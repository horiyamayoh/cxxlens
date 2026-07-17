#include <array>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/provider/clang22.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/sdk.hpp>

namespace
{
	class coverage_provider final : public cxxlens::sdk::provider::portable_provider
	{
	  public:
		std::string_view id() const noexcept override
		{
			return "company.test.coverage-provider";
		}

		cxxlens::sdk::semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}

		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task& task,
									   cxxlens::sdk::provider::context& context) override
		{
			context.coverage().request("project", task.project.catalog_id);
			if (auto classified = context.coverage().classify(
					{"project", task.project.catalog_id, "covered", {}});
				!classified)
				return classified;
			context.evidence().add(
				{"provider.observation", task.project.catalog_id, std::string{id()}, "test"});
			return {};
		}
	};

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
		require(row.has_value(), "row did not finish");
		return std::move(*row);
	}

	[[nodiscard]] cxxlens::sdk::relation_descriptor
	make_merge_descriptor(std::string name, const cxxlens::sdk::merge_mode mode)
	{
		cxxlens::sdk::relation_descriptor descriptor;
		descriptor.id = name + ".v1";
		descriptor.name = std::move(name);
		descriptor.version = {1U, 0U, 0U};
		descriptor.semantic_major = 1U;
		descriptor.semantics = descriptor.name + "/1";
		descriptor.owner_namespace = "company.test";
		descriptor.columns = {
			{descriptor.id + ".key",
			 "key",
			 {cxxlens::sdk::scalar_kind::typed_id, "company_test_id", false},
			 true,
			 cxxlens::sdk::column_role::claim_key},
			{descriptor.id + ".value",
			 "value",
			 {cxxlens::sdk::scalar_kind::utf8_string, {}, false},
			 true,
			 cxxlens::sdk::column_role::authoritative_payload},
		};
		descriptor.key_columns = {descriptor.id + ".key"};
		descriptor.merge = mode;
		if (mode == cxxlens::sdk::merge_mode::functional_assertion)
			descriptor.conflict_columns = {descriptor.id + ".value"};
		descriptor.descriptor_digest = *cxxlens::sdk::semantic_digest(
			"cxxlens.relation-descriptor-binding.v2",
			descriptor.contract_digest + "\n" + descriptor.canonical_form());
		return descriptor;
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

	[[nodiscard]] cxxlens::sdk::detached_row make_merge_row(
		const cxxlens::sdk::relation_descriptor& descriptor, std::string key, std::string value)
	{
		cxxlens::sdk::row_builder builder{descriptor};
		require(builder
					.set({descriptor.id,
						  descriptor.id + ".key",
						  {cxxlens::sdk::scalar_kind::typed_id, "company_test_id", false}},
						 cxxlens::sdk::detached_cell::typed("company_test_id", std::move(key)))
					.has_value(),
				"merge key rejected");
		require(builder
					.set({descriptor.id,
						  descriptor.id + ".value",
						  {cxxlens::sdk::scalar_kind::utf8_string, {}, false}},
						 cxxlens::sdk::detached_cell::utf8(std::move(value)))
					.has_value(),
				"merge value rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "merge row did not finish");
		return std::move(*row);
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

	void check_digest()
	{
		const std::vector<std::byte> empty;
		require(cxxlens::sdk::content_digest(empty) ==
					"sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
				"SHA-256 content digest mismatch");
		const std::string payload_a{"\0b", 2};
		const std::string invalid_domain{"a\0", 2};
		auto first = cxxlens::sdk::semantic_digest("a", payload_a);
		auto second =
			cxxlens::sdk::semantic_digest("a.b", std::string_view{"binary\0payload", 14U});
		require(first && second && *first != *second,
				"semantic digest v2 did not preserve typed domain/payload boundaries");
		require(*first ==
					"semantic-v2:sha256:"
					"0a558924559edbcc7c26286adae66eea9dc33f3e75b1c2747ef1027bf9cb68d3",
				"semantic digest v2 reference vector diverged");
		std::set<std::string> length_boundary_digests;
		for (const auto length : {0U, 1U, 255U, 256U, 65535U, 65536U})
		{
			auto digest =
				cxxlens::sdk::semantic_digest("length.boundary", std::string(length, 'x'));
			require(digest.has_value(), "semantic digest rejected a binary length boundary");
			length_boundary_digests.insert(std::move(*digest));
		}
		require(length_boundary_digests.size() == 6U,
				"semantic digest length framing was not injective at a field boundary");
		for (const auto& invalid : {std::string{}, invalid_domain, std::string{"a\n"}})
		{
			auto rejected = cxxlens::sdk::semantic_digest(invalid, "b");
			require(!rejected && rejected.error().code == "sdk.semantic-domain-invalid",
					"invalid semantic domain was accepted");
		}
	}

	void check_descriptor_binding()
	{
		using cxxlens::sdk::column_role;
		using cxxlens::sdk::merge_mode;
		using cxxlens::sdk::reference_strength;
		using cxxlens::sdk::scalar_kind;
		const auto trusted = cxxlens::cc::relations::call_site::descriptor();
		require(trusted.validate().has_value(), "generated descriptor binding rejected authority");
		const auto rejects_stale_binding =
			[](cxxlens::sdk::relation_descriptor candidate, const std::string_view subject)
		{
			auto valid = candidate.validate();
			require(!valid && valid.error().code == "sdk.descriptor-digest-mismatch",
					"generated descriptor mutation retained its trusted digest: " +
						std::string{subject});
		};

		auto merge = trusted;
		merge.merge = merge_mode::set;
		rejects_stale_binding(std::move(merge), "merge");

		auto key = trusted;
		key.columns[0].role = column_role::authoritative_payload;
		key.columns[1].role = column_role::claim_key;
		key.key_columns = {key.columns[1].id};
		rejects_stale_binding(std::move(key), "key");

		auto column_type = trusted;
		column_type.columns.back().type.scalar = scalar_kind::signed_integer;
		rejects_stale_binding(std::move(column_type), "column type");

		auto reference = trusted;
		reference.references.front().strength = reference_strength::soft_semantic;
		rejects_stale_binding(std::move(reference), "reference");

		auto semantics = trusted;
		semantics.semantics += ".forged";
		rejects_stale_binding(std::move(semantics), "semantics");

		auto conflicts = trusted;
		conflicts.conflict_columns.pop_back();
		rejects_stale_binding(std::move(conflicts), "conflict columns");

		auto set_descriptor = make_merge_descriptor("company.test.binding", merge_mode::set);
		auto multiset_descriptor =
			make_merge_descriptor("company.test.binding", merge_mode::multiset);
		cxxlens::sdk::relation_registry set_registry;
		cxxlens::sdk::relation_registry multiset_registry;
		require(set_registry.add(std::move(set_descriptor)) &&
					multiset_registry.add(std::move(multiset_descriptor)),
				"dynamic descriptor binding setup failed");
		auto set_engine = set_registry.build("binding-generation");
		auto multiset_engine = multiset_registry.build("binding-generation");
		require(set_engine && multiset_engine &&
					set_engine->registry_digest() != multiset_engine->registry_digest(),
				"different runtime descriptors produced the same registry digest");
	}

	void check_relation_schema_parity()
	{
		using cxxlens::sdk::merge_mode;
		using cxxlens::sdk::reference_strength;
		const auto rejects = [](cxxlens::sdk::relation_descriptor candidate,
								const std::string_view code,
								const std::string_view field,
								const std::string_view subject)
		{
			auto valid = candidate.validate();
			require(!valid && valid.error().code == code && valid.error().field == field,
					"schema-invalid runtime descriptor was accepted or misclassified: " +
						std::string{subject});
		};

		auto valid = make_merge_descriptor("company.schema.valid", merge_mode::set);
		require(valid.validate().has_value(), "schema-valid dynamic descriptor was rejected");
		require(cxxlens::cc::relations::call_site::descriptor().validate().has_value(),
				"schema-valid generated descriptor was rejected");

		for (const auto name : {"", "company..item", "company.1item", "company.bad-item"})
		{
			auto candidate = valid;
			candidate.name = name;
			rejects(std::move(candidate), "sdk.relation-invalid", "name", name);
		}

		auto zero_major = valid;
		zero_major.semantic_major = 0U;
		zero_major.version.major = 0U;
		zero_major.id = zero_major.name + ".v0";
		rejects(
			std::move(zero_major), "sdk.relation-invalid", "semantic_major", "semantic major 0");

		auto owner = valid;
		owner.owner_namespace = "!";
		rejects(std::move(owner), "sdk.relation-invalid", "owner_namespace", "owner namespace");

		auto semantics = valid;
		semantics.semantics = "not/a/valid/schema/value";
		rejects(std::move(semantics), "sdk.relation-invalid", "semantics", "semantics");

		auto column_name = valid;
		column_name.columns.front().name = "Invalid-Column";
		rejects(std::move(column_name), "sdk.column-invalid", "Invalid-Column", "column name");

		auto duplicate_key = valid;
		duplicate_key.key_columns.push_back(duplicate_key.key_columns.front());
		rejects(
			std::move(duplicate_key), "sdk.relation-invalid", "key_columns", "duplicate claim key");

		auto missing_role_key = valid;
		missing_role_key.columns.back().role = cxxlens::sdk::column_role::claim_key;
		rejects(std::move(missing_role_key),
				"sdk.relation-invalid",
				"key_columns",
				"claim key role parity");

		auto duplicate_source = valid;
		duplicate_source.references = {
			{{valid.columns.front().id, valid.columns.front().id},
			 "company.schema.target",
			 {"company.schema.target.v1.first", "company.schema.target.v1.second"},
			 reference_strength::soft_semantic}};
		rejects(std::move(duplicate_source),
				"sdk.reference-invalid",
				valid.name,
				"duplicate reference source columns");

		auto duplicate_target = valid;
		duplicate_target.references = {
			{{valid.columns.front().id, valid.columns.back().id},
			 "company.schema.target",
			 {"company.schema.target.v1.key", "company.schema.target.v1.key"},
			 reference_strength::hard}};
		rejects(std::move(duplicate_target),
				"sdk.reference-invalid",
				valid.name,
				"duplicate reference target columns");

		auto missing_reference_shape = valid;
		missing_reference_shape.references = {
			{{}, "company.schema.target", {}, reference_strength::hard}};
		rejects(std::move(missing_reference_shape),
				"sdk.reference-invalid",
				valid.name,
				"missing reference columns");

		auto functional =
			make_merge_descriptor("company.schema.functional", merge_mode::functional_assertion);
		functional.conflict_columns.clear();
		rejects(std::move(functional),
				"sdk.relation-invalid",
				"conflict_columns",
				"functional conflict projection");

		auto nonfunctional = valid;
		nonfunctional.conflict_columns = {nonfunctional.columns.back().id};
		rejects(std::move(nonfunctional),
				"sdk.relation-invalid",
				"conflict_columns",
				"nonfunctional conflict projection");

		auto registry_invalid = valid;
		registry_invalid.name = "company..item";
		registry_invalid.descriptor_digest.clear();
		cxxlens::sdk::relation_registry registry;
		auto added = registry.add(std::move(registry_invalid));
		require(!added && added.error().code == "sdk.relation-invalid" &&
					added.error().field == "name",
				"relation registry admitted a schema-invalid dynamic descriptor");
	}

	void check_static_dynamic_query()
	{
		using relation = cxxlens::cc::relations::call_site;
		auto typed = cxxlens::sdk::query::from<relation>();
		cxxlens::sdk::relation_registry registry;
		require(registry
					.add(make_target_descriptor(
						"build.compile_unit", "compile_unit", "compile_unit_id"))
					.has_value(),
				"compile-unit descriptor rejected");
		require(registry.add(make_target_descriptor("source.span", "span", "source_span_id"))
					.has_value(),
				"source-span descriptor rejected");
		require(registry.add(relation::descriptor()).has_value(), "descriptor registration failed");
		auto dynamic_relation = registry.require("cc.call_site", 1U);
		require(dynamic_relation.has_value(), "dynamic relation missing");
		auto dynamic = cxxlens::sdk::query::dynamic_query::from(*dynamic_relation);
		require(typed && dynamic && typed->ir().digest() == dynamic->ir().digest(),
				"static/dynamic IR diverged");
		auto wrong =
			cxxlens::sdk::query::equals_present(cxxlens::sdk::query::col<relation::ordinal>(),
												cxxlens::sdk::query::literal::utf8("zero"));
		require(!wrong && wrong.error().code == "sdk.query-literal-type-mismatch",
				"schema-aware literal mismatch was accepted");
		auto optional_present = cxxlens::sdk::query::equals_present(
			cxxlens::sdk::query::col<relation::caller>(),
			cxxlens::sdk::query::literal::typed("cc_entity_id", "entity:\"1\""));
		require(optional_present && optional_present->canonical.contains("entity:\\\"1\\\""),
				"optional present predicate was not typed/escaped");
		auto escaped = cxxlens::sdk::query::from<relation>();
		require(escaped.has_value(), "escaped query source failed");
		auto restricted = std::move(*escaped).interpretation_restrict("mode:\"debug\"\n");
		require(restricted && restricted->ir().canonical_form().contains("mode:\\\"debug\\\"\\n"),
				"logical query JSON string was not escaped");
		auto unordered = std::move(*typed).limit(1U);
		require(!unordered && unordered.error().code == "sdk.query-limit-requires-order",
				"unordered limit was accepted");
	}

	void check_snapshot_lifetime()
	{
		using relation = cxxlens::cc::relations::call_site;
		cxxlens::sdk::relation_registry registry;
		require(registry.add(relation::descriptor()).has_value(), "snapshot descriptor rejected");
		auto dynamic = registry.require("cc.call_site", 1U);
		cxxlens::sdk::snapshot_builder builder{registry};
		require(builder.add(make_call_row()).has_value(), "snapshot row rejected");
		auto snapshot = std::move(builder).publish();
		require(snapshot.has_value() && !snapshot->id().empty(), "snapshot publication failed");
		auto cursor = snapshot->open(*dynamic);
		require(cursor.has_value(), "snapshot cursor failed");
		auto first = cursor->next();
		require(first && first->has_value() && (*first)->copy().has_value(),
				"row view unavailable");
		auto end = cursor->next();
		require(end && !end->has_value(), "cursor did not end");
		auto expired = (*first)->copy();
		require(!expired && expired.error().code == "sdk.row-view-expired",
				"advanced cursor left row view live");
	}

	void check_frame_and_native_escape()
	{
		cxxlens::sdk::provider::frame frame;
		frame.type = cxxlens::sdk::provider::message_type::hello;
		frame.stream_id = 1U;
		frame.sequence = 0U;
		frame.control = {std::byte{0x60}};
		auto encoded = cxxlens::sdk::provider::encode_frame(frame);
		require(encoded && encoded->size() == 105U, "protocol header size is not 104 bytes");
		auto decoded = cxxlens::sdk::provider::decode_frame(*encoded);
		require(decoded && decoded->type == frame.type, "protocol frame did not round-trip");
		encoded->at(40U) ^= std::byte{0x01};
		auto corrupt = cxxlens::sdk::provider::decode_frame(*encoded);
		require(!corrupt && corrupt.error().code == "provider.checksum-mismatch",
				"checksum corruption was accepted");

		auto row = make_call_row();
		require(cxxlens::provider::clang22::detect_native_escape(row).has_value(),
				"detached semantic row looked native");
		row.cells.emplace("frontend.clang22.pointer",
						  cxxlens::sdk::detached_cell::utf8("0x12345678"));
		auto escaped = cxxlens::provider::clang22::detect_native_escape(row);
		require(!escaped && escaped.error().code == "native.address-escape",
				"native address marker escaped");
	}

	void check_provider_tooling_and_faults()
	{
		cxxlens::sdk::provider::coverage_builder incomplete;
		incomplete.request("project", "catalog-1");
		auto missing = std::move(incomplete).finish();
		require(!missing && missing.error().code == "provider.coverage-incomplete",
				"incomplete provider coverage was accepted");

		auto portable = cxxlens::sdk::provider::make_scaffold(
			{"company.example.provider", "portable", "company.example.relation"});
		auto native = cxxlens::sdk::provider::make_scaffold(
			{"company.example.native", "clang22-native", "company.example.relation"});
		require(portable && native && portable->size() == 5U && native->size() == 5U,
				"provider scaffold is incomplete");
		const auto portable_manifest =
			std::ranges::find(*portable,
							  std::string{"provider-manifest.json"},
							  &cxxlens::sdk::provider::scaffold_file::relative_path);
		const auto native_cmake =
			std::ranges::find(*native,
							  std::string{"CMakeLists.txt"},
							  &cxxlens::sdk::provider::scaffold_file::relative_path);
		require(portable_manifest != portable->end() &&
					portable_manifest->content.contains("cxxlens.provider-manifest.v1") &&
					native_cmake != native->end() &&
					native_cmake->content.contains("cxxlens::clang22_provider_sdk"),
				"provider scaffold package/manifest contract diverged");

		coverage_provider implementation;
		cxxlens::sdk::provider::task task;
		task.task_id = "task-1";
		task.project = {"catalog-1", "sha256:catalog", ".", {"unit-1"}};
		task.outputs = {cxxlens::cc::relations::call_site::descriptor()};
		task.condition = "condition:all";
		task.interpretation = "company.test";
		cxxlens::sdk::testing::provider_harness harness;
		auto accepted = harness.run(implementation, task);
		auto corrupt = harness.run(
			implementation, task, cxxlens::sdk::testing::provider_fault::corrupt_checksum);
		auto truncated = harness.run(
			implementation, task, cxxlens::sdk::testing::provider_fault::truncate_last_frame);
		auto cancelled = harness.run(
			implementation, task, cxxlens::sdk::testing::provider_fault::cancel_before_run);
		require(accepted && accepted->accepted && corrupt && !corrupt->accepted &&
					corrupt->reason_code == "provider.checksum-mismatch" && truncated &&
					!truncated->accepted && truncated->reason_code == "provider.truncated-stream" &&
					cancelled && !cancelled->accepted &&
					cancelled->reason_code == "provider.cancelled",
				"provider harness fault matrix diverged");
	}

	void check_relation_engine_and_claim_kernel()
	{
		using cxxlens::sdk::merge_mode;
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
		require(registry.add(cxxlens::company::relations::lock_acquire::descriptor()).has_value(),
				"external descriptor rejected");
		std::array descriptors{
			make_merge_descriptor("company.test.set", merge_mode::set),
			make_merge_descriptor("company.test.multiset", merge_mode::multiset),
			make_merge_descriptor("company.test.functional", merge_mode::functional_assertion),
			make_merge_descriptor("company.test.keyed_union", merge_mode::keyed_union),
		};
		for (const auto& descriptor : descriptors)
			require(registry.add(descriptor).has_value(), "merge descriptor rejected");

		auto registry_copy = registry;
		auto engine = registry.build("engine-generation-1");
		require(engine && !engine->registry_digest().empty() && registry.frozen(),
				"registry did not publish an immutable engine");
		auto late = registry.add(make_merge_descriptor("company.test.late", merge_mode::set));
		require(!late && late.error().code == "sdk.registry-frozen",
				"registry mutation after engine build was accepted");
		auto late_copy =
			registry_copy.add(make_merge_descriptor("company.test.late-copy", merge_mode::set));
		require(!late_copy && late_copy.error().code == "sdk.registry-frozen",
				"a registry copy escaped engine-build freezing");
		auto dynamic = engine->require("cc.call_site", 1U);
		require(dynamic &&
					dynamic->descriptor().descriptor_digest ==
						cxxlens::cc::relations::call_site::descriptor().descriptor_digest,
				"static/dynamic descriptor digest diverged");
		auto universe_mismatch = cxxlens::sdk::claim_condition{"universe:a", {"all"}}.overlap(
			cxxlens::sdk::claim_condition{"universe:b", {"all"}});
		require(!universe_mismatch &&
					universe_mismatch.error().code == "sdk.condition-universe-mismatch",
				"different condition universes were silently compared");

		auto first =
			cxxlens::sdk::make_assertion(*engine, observe(make_direct_target_row("entity:a")));
		auto repeated =
			cxxlens::sdk::make_assertion(*engine, observe(make_direct_target_row("entity:a")));
		require(first && repeated && first->semantic_key == repeated->semantic_key &&
					first->assertion == repeated->assertion && first->content == repeated->content,
				"claim identity depended on arrival, jobs, or runtime root");
		auto malformed_observation = observe(make_direct_target_row("entity:a"));
		malformed_observation.input_basis.basis_digest = "sha256:not-a-digest";
		auto malformed = cxxlens::sdk::make_assertion(*engine, std::move(malformed_observation));
		require(!malformed && malformed.error().code == "sdk.claim-basis-invalid",
				"malformed direct claim basis was accepted");
		auto tampered = *first;
		tampered.content.back() = tampered.content.back() == '0' ? '1' : '0';
		auto tampered_result = cxxlens::sdk::validate_claim(*engine, tampered);
		require(!tampered_result && tampered_result.error().code == "sdk.claim-identity-mismatch",
				"tampered claim identity was accepted");
		const auto call_row_for_view = make_call_row();
		cxxlens::cc::relations::call_site::view typed_view{call_row_for_view};
		auto absent_caller = typed_view.get<cxxlens::cc::relations::call_site::caller>();
		require(absent_caller && absent_caller->state == cxxlens::sdk::cell_state::absent,
				"typed view collapsed an optional absent cell into failure");
		auto canonical = cxxlens::sdk::make_canonical_claim(
			*engine,
			*first,
			{"company.test.canonicalizer", "company.test.canonicalizer.v1"},
			make_direct_target_row("entity:a"),
			"sha256:1111111111111111111111111111111111111111111111111111111111111111");
		require(canonical && canonical->stage == cxxlens::sdk::claim_stage::canonical_claim &&
					cxxlens::sdk::validate_claim(*engine, *canonical).has_value(),
				"canonical claim stage failed independent validation");
		const std::array canonical_inputs{*canonical};
		auto derived = cxxlens::sdk::make_derived_claim(
			*engine,
			canonical_inputs,
			observe(make_direct_target_row("entity:a")),
			"snapshot:1",
			{"partition-content:sha256:"
			 "3333333333333333333333333333333333333333333333333333333333333333"},
			"sha256:2222222222222222222222222222222222222222222222222222222222222222");
		require(derived && derived->stage == cxxlens::sdk::claim_stage::derived_claim &&
					cxxlens::sdk::validate_claim(*engine, *derived).has_value(),
				"derived claim stage failed independent validation");

		auto call = cxxlens::sdk::make_assertion(
			*engine, observe(make_call_row(), {"all", "asan", "debug", "release"}));
		require(call.has_value(), "call-site assertion rejected");
		cxxlens::sdk::claim_batch hard_missing;
		require(hard_missing.add(*first).has_value(), "hard-reference candidate rejected");
		auto rejected = std::move(hard_missing).commit(*engine);
		require(!rejected && rejected.error().code == "sdk.hard-reference-missing",
				"hard missing reference did not reject the batch");

		cxxlens::sdk::claim_batch soft_missing;
		require(soft_missing.add(*first).has_value(), "soft-reference candidate rejected");
		const std::array existing_call{*call};
		auto accepted = std::move(soft_missing).commit(*engine, existing_call);
		require(accepted && accepted->claims.size() == 1U && accepted->unresolved.size() == 1U &&
					accepted->unresolved.front().target_relation == "cc.entity",
				"soft missing reference did not preserve row and unresolved evidence");

		auto overlap_a = cxxlens::sdk::make_assertion(
			*engine, observe(make_direct_target_row("entity:a"), {"debug", "release"}));
		auto overlap_b = cxxlens::sdk::make_assertion(
			*engine, observe(make_direct_target_row("entity:b"), {"release"}));
		auto same_payload = cxxlens::sdk::make_assertion(
			*engine, observe(make_direct_target_row("entity:a"), {"release"}));
		auto disjoint = cxxlens::sdk::make_assertion(
			*engine, observe(make_direct_target_row("entity:c"), {"asan"}));
		auto other_domain = cxxlens::sdk::make_assertion(
			*engine,
			observe(make_direct_target_row("entity:d"), {"release"}, "company.other.domain"));
		auto other_call = cxxlens::sdk::make_assertion(
			*engine,
			observe(make_call_row(), {"all", "asan", "debug", "release"}, "company.other.domain"));
		require(overlap_a && overlap_b && same_payload && disjoint && other_domain && other_call,
				"functional claims could not be constructed");
		cxxlens::sdk::claim_batch matching_payload;
		require(matching_payload.add(*overlap_a) && matching_payload.add(*same_payload),
				"matching payload claims were rejected");
		auto matching = std::move(matching_payload).commit(*engine, existing_call);
		require(matching && matching->conflicts.empty(),
				"condition-dependent content IDs caused a false payload conflict");
		cxxlens::sdk::claim_batch comparisons;
		for (const auto* value : {&*overlap_a, &*overlap_b, &*disjoint, &*other_domain})
			require(comparisons.add(*value).has_value(), "comparison claim rejected");
		const std::array comparison_targets{*call, *other_call};
		auto compared = std::move(comparisons).commit(*engine, comparison_targets);
		require(compared && compared->conflicts.size() == 1U &&
					compared->conflicts.front().overlap_fragments ==
						std::vector<std::string>{"release"} &&
					compared->differential_disagreements.size() == 2U,
				"condition overlap or interpretation-domain disagreement was misclassified: "
				"conflicts=" +
					std::to_string(compared ? compared->conflicts.size() : 999U) +
					", differential=" +
					std::to_string(compared ? compared->differential_disagreements.size() : 999U));

		for (const auto& descriptor : descriptors)
		{
			auto left = cxxlens::sdk::make_assertion(
				*engine, observe(make_merge_row(descriptor, "key:1", "value")));
			auto right = cxxlens::sdk::make_assertion(
				*engine, observe(make_merge_row(descriptor, "key:1", "value")));
			require(left && right, "merge-mode assertions rejected");
			cxxlens::sdk::claim_batch batch;
			require(batch.add(*left).has_value() && batch.add(*right).has_value(),
					"merge-mode batch rejected");
			auto result = std::move(batch).commit(*engine);
			const auto expected = descriptor.merge == merge_mode::multiset ? 2U : 1U;
			require(result && result->claims.size() == expected, "merge law was not applied");
		}

		auto native = make_merge_descriptor("company.test.native", merge_mode::set);
		native.columns.back().type = {cxxlens::sdk::scalar_kind::typed_id, "native_pointer", false};
		native.descriptor_digest =
			*cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
										   native.contract_digest + "\n" + native.canonical_form());
		cxxlens::sdk::relation_registry native_registry;
		auto native_rejected = native_registry.add(std::move(native));
		require(!native_rejected && native_rejected.error().code == "sdk.native-address-payload",
				"native pointer/address payload type was accepted");

		auto cycle_a = make_merge_descriptor("company.test.cycle_a", merge_mode::set);
		auto cycle_b = make_merge_descriptor("company.test.cycle_b", merge_mode::set);
		cycle_a.references = {{{cycle_a.id + ".key"},
							   cycle_b.name,
							   {cycle_b.id + ".key"},
							   cxxlens::sdk::reference_strength::hard}};
		cycle_b.references = {{{cycle_b.id + ".key"},
							   cycle_a.name,
							   {cycle_a.id + ".key"},
							   cxxlens::sdk::reference_strength::hard}};
		for (auto* descriptor : {&cycle_a, &cycle_b})
			descriptor->descriptor_digest = *cxxlens::sdk::semantic_digest(
				"cxxlens.relation-descriptor-binding.v2",
				descriptor->contract_digest + "\n" + descriptor->canonical_form());
		cxxlens::sdk::relation_registry cycle_registry;
		require(cycle_registry.add(cycle_a) && cycle_registry.add(cycle_b),
				"hard-cycle descriptors were rejected before engine validation");
		auto cycle = cycle_registry.build("cycle-generation");
		require(!cycle && cycle.error().code == "sdk.hard-reference-cycle",
				"hard reference cycle was accepted");

		cxxlens::sdk::relation_registry missing_target_registry;
		require(missing_target_registry.add(cycle_a).has_value(),
				"missing-target descriptor rejected early");
		auto missing_target = missing_target_registry.build("missing-target-generation");
		require(!missing_target && missing_target.error().code == "sdk.registry-reference-missing",
				"missing hard target descriptor was accepted");
	}
} // namespace

int main()
{
	check_digest();
	check_descriptor_binding();
	check_relation_schema_parity();
	check_static_dynamic_query();
	check_snapshot_lifetime();
	check_frame_and_native_escape();
	check_provider_tooling_and_faults();
	check_relation_engine_and_claim_kernel();
	return 0;
}
