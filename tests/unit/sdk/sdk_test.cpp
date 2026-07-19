#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/provider/clang22.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/sdk.hpp>

namespace
{
	static_assert(!std::is_copy_constructible_v<cxxlens::sdk::provider::relation_sink>);
	static_assert(std::is_nothrow_move_constructible_v<cxxlens::sdk::provider::relation_sink>);
	static_assert(!std::is_move_assignable_v<cxxlens::sdk::provider::relation_sink>);
	template <typename Value>
	concept has_detached_evidence_references = requires(Value value) { value.evidence; };
	static_assert(!has_detached_evidence_references<cxxlens::sdk::claim>,
				  "claim evidence occurrence must remain structurally self-contained");
	static_assert(!has_detached_evidence_references<cxxlens::sdk::claim_batch_result>,
				  "claim batch must not expose orphanable detached evidence records");

	constexpr std::string_view provider_contract_digest{
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};

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
		std::string_view semantic_contract_digest() const noexcept override
		{
			return provider_contract_digest;
		}

		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task& task,
									   cxxlens::sdk::provider::context& context) override
		{
			context.coverage().request("task", task.task_id);
			if (auto classified =
					context.coverage().classify({"task", task.task_id, "covered", {}});
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

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] cxxlens::sdk::catalog_compile_unit catalog_unit(std::string id, const char seed)
	{
		const auto next = [](const char value)
		{
			return value == 'f' ? '0' : static_cast<char>(value + 1);
		};
		return {std::move(id), digest(seed), digest(next(seed)), digest(next(next(seed)))};
	}

	[[nodiscard]] cxxlens::sdk::project_catalog
	make_catalog(std::string root, std::vector<cxxlens::sdk::catalog_compile_unit> units)
	{
		auto catalog =
			cxxlens::sdk::project_catalog::make(std::move(root), digest('d'), std::move(units));
		require(catalog.has_value(), "valid project catalog was rejected");
		return std::move(*catalog);
	}

	[[nodiscard]] cxxlens::sdk::provider::task
	make_provider_task(cxxlens::sdk::provider::portable_provider& provider,
					   cxxlens::sdk::project_catalog project,
					   std::vector<cxxlens::sdk::relation_descriptor> outputs,
					   std::string condition = "condition:all",
					   std::string interpretation = "company.test",
					   std::vector<std::string> dependency_groups = {"dependency-1"})
	{
		auto task =
			cxxlens::sdk::provider::task::make({std::string{provider.id()},
												provider.version(),
												std::string{provider.semantic_contract_digest()},
												outputs,
												{},
												{interpretation},
												"observation",
												"assertion"},
											   std::move(project),
											   std::move(outputs),
											   std::move(condition),
											   std::move(interpretation),
											   std::move(dependency_groups));
		require(task.has_value(), "valid provider task was rejected");
		return std::move(*task);
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

	[[nodiscard]] std::string structured_control_text()
	{
		std::string value{"pipe|line\nreturn\r"};
		value.push_back('\0');
		value += "雪";
		return value;
	}

	class structured_metadata_provider final : public cxxlens::sdk::provider::portable_provider
	{
	  public:
		explicit structured_metadata_provider(const bool alternate_summary)
			: alternate_summary_{alternate_summary}
		{
		}

		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "company.test.structured-metadata-provider";
		}
		[[nodiscard]] cxxlens::sdk::semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		[[nodiscard]] std::string_view semantic_contract_digest() const noexcept override
		{
			return provider_contract_digest;
		}
		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task& task,
									   cxxlens::sdk::provider::context& context) override
		{
			const auto special = structured_control_text();
			auto output = context.relation(cxxlens::cc::relations::call_site::descriptor());
			if (auto begun = output.begin(task.dependency_groups.front(), special, special); !begun)
				return begun;
			if (auto pushed = output.push(make_call_row()); !pushed)
				return pushed;
			if (auto ended = output.end(); !ended)
				return ended;
			context.coverage().request("task", task.task_id);
			if (auto classified =
					context.coverage().classify({"task", task.task_id, "covered", special});
				!classified)
				return classified;
			context.unresolved().add({"provider.delimiter", special, special});
			context.evidence().add(
				{"provider.evidence", special, std::string{id()}, "summary-A" + special});
			context.evidence().add({"provider.evidence",
									special,
									std::string{id()},
									(alternate_summary_ ? "summary-C" : "summary-B") + special});
			return {};
		}

	  private:
		bool alternate_summary_{};
	};

	[[nodiscard]] cxxlens::sdk::detached_row make_lock_row()
	{
		using relation = cxxlens::company::relations::lock_acquire;
		relation::builder builder;
		require(
			builder.set<relation::acquire>(
				cxxlens::sdk::detached_cell::typed("company_lock_acquire_id", "acquire:1")) &&
				builder.set<relation::lock>(
					cxxlens::sdk::detached_cell::typed("company_lock_id", "lock:1")) &&
				builder.set<relation::source>(
					cxxlens::sdk::detached_cell::typed("source_span_id", "span:1")) &&
				builder.set<relation::mode>(string_cell(
					{cxxlens::sdk::scalar_kind::open_symbol, "company.lock-mode/1", false},
					"exclusive")) &&
				builder.set<relation::ordinal>(cxxlens::sdk::detached_cell::unsigned_integer(0U)),
			"lock row setup failed");
		auto row = std::move(builder).finish();
		require(row.has_value(), "lock row did not finish");
		return std::move(*row);
	}

	class unsealed_provider final : public cxxlens::sdk::provider::portable_provider
	{
	  public:
		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "company.test.unsealed-provider";
		}
		[[nodiscard]] cxxlens::sdk::semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		std::string_view semantic_contract_digest() const noexcept override
		{
			return provider_contract_digest;
		}
		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task& task,
									   cxxlens::sdk::provider::context& context) override
		{
			auto output = context.relation(cxxlens::cc::relations::call_site::descriptor());
			if (auto begun = output.begin("dependency-1", "atomic-1", "batch-unsealed"); !begun)
				return begun;
			auto moved_output = std::move(output);
			if (auto pushed = moved_output.push(make_call_row()); !pushed)
				return pushed;
			context.coverage().request("task", task.task_id);
			return context.coverage().classify({"task", task.task_id, "covered", {}});
		}
	};

	enum class batch_lifecycle_scenario : std::uint8_t
	{
		begin_only,
		partially_unsealed,
		duplicate_batch,
		interleaved_sink,
		complete,
	};

	class batch_lifecycle_provider final : public cxxlens::sdk::provider::portable_provider
	{
	  public:
		batch_lifecycle_provider(std::string provider_id, const batch_lifecycle_scenario scenario)
			: provider_id_{std::move(provider_id)}, scenario_{scenario}
		{
		}
		[[nodiscard]] std::string_view id() const noexcept override
		{
			return provider_id_;
		}
		[[nodiscard]] cxxlens::sdk::semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		std::string_view semantic_contract_digest() const noexcept override
		{
			return provider_contract_digest;
		}
		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task& task,
									   cxxlens::sdk::provider::context& context) override
		{
			const auto descriptor = cxxlens::cc::relations::call_site::descriptor();
			auto first = context.relation(descriptor);
			if (auto begun = first.begin("dependency-1", "atomic-1", "batch-shared"); !begun)
				return begun;
			if (scenario_ == batch_lifecycle_scenario::begin_only)
				return cover(task, context);
			if (auto pushed = first.push(make_call_row()); !pushed)
				return pushed;
			if (scenario_ == batch_lifecycle_scenario::interleaved_sink)
			{
				auto second = context.relation(descriptor);
				return second.begin("dependency-1", "atomic-2", "batch-interleaved");
			}
			if (auto ended = first.end(); !ended)
				return ended;
			if (scenario_ == batch_lifecycle_scenario::duplicate_batch)
			{
				auto second = context.relation(descriptor);
				return second.begin("dependency-1", "atomic-2", "batch-shared");
			}
			auto second = context.relation(descriptor);
			if (auto begun = second.begin("dependency-1", "atomic-2", "batch-second"); !begun)
				return begun;
			if (auto pushed = second.push(make_call_row()); !pushed)
				return pushed;
			if (scenario_ == batch_lifecycle_scenario::partially_unsealed)
				return cover(task, context);
			if (auto ended = second.end(); !ended)
				return ended;
			return cover(task, context);
		}

	  private:
		static cxxlens::sdk::result<void> cover(const cxxlens::sdk::provider::task& task,
												cxxlens::sdk::provider::context& context)
		{
			context.coverage().request("task", task.task_id);
			return context.coverage().classify({"task", task.task_id, "covered", {}});
		}

		std::string provider_id_;
		batch_lifecycle_scenario scenario_;
	};

	class unrequested_descriptor_provider final : public cxxlens::sdk::provider::portable_provider
	{
	  public:
		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "company.test.unrequested-provider";
		}
		[[nodiscard]] cxxlens::sdk::semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		std::string_view semantic_contract_digest() const noexcept override
		{
			return provider_contract_digest;
		}
		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task& task,
									   cxxlens::sdk::provider::context& context) override
		{
			auto output = context.relation(cxxlens::company::relations::lock_acquire::descriptor());
			if (auto begun = output.begin("dependency-1", "atomic-1", "batch-unrequested"); !begun)
				return begun;
			if (auto pushed = output.push(make_lock_row()); !pushed)
				return pushed;
			if (auto ended = output.end(); !ended)
				return ended;
			context.coverage().request("task", task.task_id);
			return context.coverage().classify({"task", task.task_id, "covered", {}});
		}
	};

	class incomplete_coverage_provider final : public cxxlens::sdk::provider::portable_provider
	{
	  public:
		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "company.test.incomplete-coverage-provider";
		}
		[[nodiscard]] cxxlens::sdk::semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		std::string_view semantic_contract_digest() const noexcept override
		{
			return provider_contract_digest;
		}
		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task& task,
									   cxxlens::sdk::provider::context& context) override
		{
			context.coverage().request("task", task.task_id);
			return {};
		}
	};

	class invalid_identity_provider final : public cxxlens::sdk::provider::portable_provider
	{
	  public:
		explicit invalid_identity_provider(const bool invalid_version)
			: invalid_version_{invalid_version}
		{
		}
		std::string_view id() const noexcept override
		{
			return invalid_version_ ? "company.test.invalid-version" : "";
		}
		cxxlens::sdk::semantic_version version() const noexcept override
		{
			return invalid_version_ ? cxxlens::sdk::semantic_version{}
									: cxxlens::sdk::semantic_version{1U, 0U, 0U};
		}
		std::string_view semantic_contract_digest() const noexcept override
		{
			return provider_contract_digest;
		}
		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task&,
									   cxxlens::sdk::provider::context&) override
		{
			return {};
		}

	  private:
		bool invalid_version_{};
	};

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

	void check_scalar_value_validation()
	{
		using namespace cxxlens::sdk;
		const auto present_text =
			[](const scalar_kind kind, const std::string_view parameter, std::string value)
		{
			return detached_cell{{kind, std::string{parameter}, false},
								 cell_state::present,
								 scalar_value{std::move(value)},
								 std::nullopt};
		};
		const std::string sha =
			"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
		const std::string semantic =
			"semantic-v2:sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
		require(present_text(scalar_kind::digest, {}, sha).validate().has_value() &&
					present_text(scalar_kind::digest, {}, semantic).validate().has_value(),
				"canonical digest scalar was rejected");
		for (const auto* invalid :
			 {"not-a-digest",
			  "sha256:ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
			  "sha256:0123"})
			require(!present_text(scalar_kind::digest, {}, invalid).validate(),
					"invalid digest scalar was accepted");

		for (const auto* valid : {"0.0.0", "1.2.3", "4294967295.0.7"})
			require(present_text(scalar_kind::semantic_version, {}, valid).validate().has_value(),
					"canonical semantic version was rejected");
		for (const auto* invalid : {"anything", "01.2.3", "1.2", "1.2.3.4", "4294967296.0.0"})
			require(!present_text(scalar_kind::semantic_version, {}, invalid).validate(),
					"invalid semantic version was accepted");

		require(present_text(scalar_kind::closed_symbol, "claim-stage/1", "assertion")
					.validate()
					.has_value(),
				"known closed symbol was rejected");
		require(
			!present_text(scalar_kind::closed_symbol, "claim-stage/1", "future_claim_stage")
					.validate() &&
				!present_text(scalar_kind::closed_symbol, "company.unknown/1", "known").validate(),
			"unknown closed symbol or contract was accepted");
		require(present_text(scalar_kind::open_symbol, "cc.call-kind/1", "vendor_future_call")
					.validate()
					.has_value(),
				"unknown open symbol was not preserved");

		for (const auto& invalid_utf8 : {std::string{"\x80", 1U},
										 std::string{"\xc0\xaf", 2U},
										 std::string{"\xe2\x82", 2U},
										 std::string{"\xed\xa0\x80", 3U},
										 std::string{"\xf4\x90\x80\x80", 4U}})
			require(!detached_cell::utf8(invalid_utf8).validate(),
					"invalid UTF-8 scalar sequence was accepted");
		require(detached_cell::utf8("valid-😀-雪").validate().has_value(),
				"valid non-BMP UTF-8 scalar was rejected");

		for (const auto& invalid : {std::string{}, std::string{"id\nvalue"}})
			require(!detached_cell::typed("cc_entity_id", invalid).validate(),
					"empty/control-containing typed ID was accepted");
		require(!detached_cell::typed("not_an_identity_type", "entity:1").validate(),
				"typed ID parameter without _id was accepted");
		require(!detached_cell::unknown({scalar_kind::utf8_string, {}, true}, "reason\nnext")
					 .validate(),
				"control-containing unknown reason was accepted");

		relation_descriptor descriptor;
		descriptor.id = "company.scalar_validation.v1";
		descriptor.name = "company.scalar_validation";
		descriptor.version = {1U, 0U, 0U};
		descriptor.semantic_major = 1U;
		descriptor.semantics = "company.scalar-validation/1";
		descriptor.owner_namespace = "company.scalar";
		descriptor.columns = {
			{descriptor.id + ".key",
			 "key",
			 {scalar_kind::typed_id, "scalar_validation_id", false},
			 true,
			 column_role::claim_key},
			{descriptor.id + ".digest",
			 "digest",
			 {scalar_kind::digest, {}, false},
			 true,
			 column_role::authoritative_payload},
		};
		descriptor.key_columns = {descriptor.columns.front().id};
		descriptor.merge = merge_mode::set;
		descriptor.descriptor_digest = *semantic_digest("cxxlens.relation-descriptor-binding.v2",
														"\n" + descriptor.canonical_form());
		require(descriptor.validate().has_value(), "scalar validation descriptor rejected");
		row_builder builder{descriptor};
		require(
			builder
				.set(
					{descriptor.id, descriptor.columns.front().id, descriptor.columns.front().type},
					detached_cell::typed("scalar_validation_id", "scalar:1"))
				.has_value(),
			"scalar validation row key rejected");
		auto rejected = builder.set(
			{descriptor.id, descriptor.columns.back().id, descriptor.columns.back().type},
			present_text(scalar_kind::digest, {}, "not-a-digest"));
		require(!rejected && rejected.error().detail == "digest",
				"row builder accepted an invalid digest scalar");
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

	void check_closed_enum_and_canonical_value()
	{
		using namespace cxxlens::sdk;
		const auto rejects_out_of_range = [](const auto last, const std::string_view subject)
		{
			using enum_type = decltype(last);
			using underlying = std::underlying_type_t<enum_type>;
			const auto adjacent = static_cast<enum_type>(static_cast<underlying>(last) + 1U);
			const auto maximum = static_cast<enum_type>(std::numeric_limits<underlying>::max());
			require(!is_valid(adjacent) && !is_valid(maximum),
					"closed enum accepted an out-of-range value: " + std::string{subject});
		};

		rejects_out_of_range(canonical_value::kind::ordered_tuple, "canonical_value::kind");
		rejects_out_of_range(scalar_kind::set, "scalar_kind");
		rejects_out_of_range(column_role::auxiliary, "column_role");
		rejects_out_of_range(merge_mode::keyed_union, "merge_mode");
		rejects_out_of_range(reference_strength::soft_semantic, "reference_strength");
		rejects_out_of_range(cell_state::unknown, "cell_state");
		rejects_out_of_range(claim_stage::derived_claim, "claim_stage");
		rejects_out_of_range(publication_state::rolled_back, "publication_state");
		rejects_out_of_range(query::column_availability::absent_if_schema_missing,
							 "column_availability");
		rejects_out_of_range(query::predicate_kind::any, "predicate_kind");
		rejects_out_of_range(query::execution_checkpoint::phase::before_publish_row,
							 "execution_checkpoint::phase");
		rejects_out_of_range(query::execution_status::failed_before_result, "execution_status");
		rejects_out_of_range(provider::frame_flag::end_of_stream, "frame_flag");
		rejects_out_of_range(provider::sandbox_assurance::certified, "sandbox_assurance");
		rejects_out_of_range(provider::discovery_source::system_registry, "discovery_source");
		rejects_out_of_range(provider::fallback_direction::same_version_rebuild,
							 "fallback_direction");
		rejects_out_of_range(provider::process_status::launch_failed, "process_status");
		rejects_out_of_range(testing::provider_fault::credit_exceeded, "provider_fault");
		rejects_out_of_range(cxxlens::recipes::call_search_state::failed, "call_search_state");

		auto invalid_cell = detached_cell::unknown({scalar_kind::utf8_string, {}, true}, "reason");
		invalid_cell.state = static_cast<cell_state>(255U);
		auto cell_result = invalid_cell.validate();
		require(!cell_result && cell_result.error().code == "sdk.cell-invalid" &&
					cell_result.error().detail == "closed-enum",
				"invalid cell state was treated as unknown");

		const auto trusted = cxxlens::cc::relations::call_site::descriptor();
		const auto descriptor_rejects =
			[](relation_descriptor candidate, const std::string_view expected_code)
		{
			auto valid = candidate.validate();
			require(!valid && valid.error().code == expected_code &&
						valid.error().detail == "closed-enum",
					"descriptor enum was canonicalized before membership validation");
		};
		auto scalar = trusted;
		scalar.columns.front().type.scalar = static_cast<scalar_kind>(255U);
		descriptor_rejects(std::move(scalar), "sdk.column-invalid");
		auto role = trusted;
		role.columns.front().role = static_cast<column_role>(255U);
		descriptor_rejects(std::move(role), "sdk.column-invalid");
		auto merge = trusted;
		merge.merge = static_cast<merge_mode>(255U);
		descriptor_rejects(std::move(merge), "sdk.relation-invalid");
		auto reference = trusted;
		reference.references.front().strength = static_cast<reference_strength>(255U);
		descriptor_rejects(std::move(reference), "sdk.reference-invalid");
		require(value_type{static_cast<scalar_kind>(254U), {}, false}.canonical_name() !=
					value_type{static_cast<scalar_kind>(255U), {}, false}.canonical_name(),
				"invalid scalar kinds collapsed or threw during canonicalization");

		provider::sandbox_requirement requirement;
		requirement.minimum = static_cast<provider::sandbox_assurance>(255U);
		auto requirement_result = requirement.validate();
		require(!requirement_result && requirement_result.error().field == "minimum",
				"invalid sandbox minimum reached ordinal comparison");
		provider::sandbox_report report;
		report.achieved = static_cast<provider::sandbox_assurance>(255U);
		auto report_result = report.validate();
		require(!report_result && report_result.error().field == "achieved",
				"invalid achieved sandbox assurance was accepted");

		const auto valid = canonical_value::from_tuple(
			{canonical_value::null(), canonical_value::from_string("valid-😀")});
		auto first = canonical_binary(valid);
		auto second = canonical_binary(valid);
		require(first && second && valid == valid && *first == *second,
				"equal canonical values did not have equal canonical bytes");
		auto decoded = canonical_binary_decode(*first);
		auto round_trip =
			decoded ? canonical_binary(*decoded) : result<std::vector<std::byte>>{decoded.error()};
		require(decoded && round_trip && *decoded == valid && *round_trip == *first,
				"canonical binary decode/encode round trip changed bytes");
		for (const auto& invalid_binary : {
				 std::vector<std::byte>{std::byte{0xff}},
				 std::vector<std::byte>{std::byte{0x00}, std::byte{0x00}},
				 std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}},
			 })
		{
			auto rejected = canonical_binary_decode(invalid_binary);
			require(!rejected && rejected.error().code == "sdk.canonical-value-invalid",
					"invalid canonical binary was decoded");
		}

		canonical_value invalid_kind;
		invalid_kind.type = static_cast<canonical_value::kind>(255U);
		canonical_value adjacent_kind = invalid_kind;
		adjacent_kind.type = static_cast<canonical_value::kind>(6U);
		canonical_value inactive;
		inactive.text = "hidden";
		canonical_value invalid_utf8 = canonical_value::from_string(std::string{"\xc0\xaf", 2U});
		canonical_value nested = canonical_value::from_tuple({invalid_kind});
		for (const auto& candidate : {invalid_kind, adjacent_kind, inactive, invalid_utf8, nested})
		{
			auto encoded = canonical_binary(candidate);
			require(!encoded && encoded.error().code == "sdk.canonical-value-invalid",
					"invalid canonical value entered identity bytes");
		}
		const std::array invalid_fields{nested};
		auto identity = canonical_identity_digest("invalid-corpus", invalid_fields);
		require(!identity && identity.error().code == "sdk.canonical-value-invalid",
				"nested invalid canonical value entered identity digest");

		const std::array invalid_utf8_corpus{
			std::string{"\xc0\xaf", 2U},
			std::string{"\xe0\x80\x80", 3U},
			std::string{"\xed\xa0\x80", 3U},
			std::string{"\xf4\x90\x80\x80", 4U},
			std::string{"\xe2\x82", 2U},
			std::string{"\xc3\x28", 2U},
		};
		for (const auto& text : invalid_utf8_corpus)
		{
			require(!validate_utf8_text(text) && !canonical_utf8_string(text) &&
						!canonical_json_text(text),
					"invalid UTF-8 was repaired or admitted by a checked text surface");
		}
		const std::string max_strong_id(512U, 'a');
		require(validate_strong_id(max_strong_id) && !validate_strong_id(max_strong_id + "a") &&
					!validate_strong_id(std::string{"bad\0id", 6U}) &&
					!validate_strong_id("bad\nidentity") &&
					validate_strong_id("delimiter|is-framed") &&
					validate_registered_symbol("schema_validated") &&
					!validate_registered_symbol("schema|validated"),
				"strong ID or registered symbol schema boundary diverged");
		auto json = canonical_json_text("valid-😀\ntext");
		require(json && *json == "\"valid-😀\\ntext\"",
				"checked canonical JSON changed valid UTF-8 meaning");
		require(!source_span_identity(std::string{"bad\xff", 4U}, "file.cpp", 0U, 1U, "token"),
				"source span identity admitted invalid UTF-8");
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
		reference.references.front().strength =
			reference.references.front().strength == reference_strength::hard
			? reference_strength::soft_semantic
			: reference_strength::hard;
		rejects_stale_binding(std::move(reference), "reference");

		auto semantics = trusted;
		semantics.semantics += ".forged";
		rejects_stale_binding(std::move(semantics), "semantics");

		auto conflicts = trusted;
		conflicts.conflict_columns.pop_back();
		rejects_stale_binding(std::move(conflicts), "conflict columns");

		auto domain_projection = trusted;
		std::ranges::reverse(domain_projection.domain_identity.projection);
		rejects_stale_binding(std::move(domain_projection), "domain identity projection");

		auto set_descriptor = make_merge_descriptor("company.test.binding", merge_mode::set);
		auto multiset_descriptor =
			make_merge_descriptor("company.test.binding", merge_mode::multiset);
		const auto expected_set_registry_digest = cxxlens::sdk::semantic_digest(
			"cxxlens.relation-registry.v1",
			set_descriptor.id + "=" + set_descriptor.descriptor_digest + "\n");
		const auto expected_multiset_registry_digest = cxxlens::sdk::semantic_digest(
			"cxxlens.relation-registry.v1",
			multiset_descriptor.id + "=" + multiset_descriptor.descriptor_digest + "\n");
		const auto legacy_name_registry_digest = cxxlens::sdk::semantic_digest(
			"cxxlens.relation-registry.v1",
			set_descriptor.name + "=" + set_descriptor.descriptor_digest + "\n");
		cxxlens::sdk::relation_registry set_registry;
		cxxlens::sdk::relation_registry multiset_registry;
		require(set_registry.add(std::move(set_descriptor)) &&
					multiset_registry.add(std::move(multiset_descriptor)),
				"dynamic descriptor binding setup failed");
		auto set_engine = set_registry.build("binding-generation");
		auto multiset_engine = multiset_registry.build("binding-generation");
		require(expected_set_registry_digest && expected_multiset_registry_digest &&
					legacy_name_registry_digest && set_engine && multiset_engine &&
					set_engine->registry_digest() == *expected_set_registry_digest &&
					multiset_engine->registry_digest() == *expected_multiset_registry_digest &&
					set_engine->registry_digest() != *legacy_name_registry_digest &&
					set_engine->registry_digest() != multiset_engine->registry_digest(),
				"registry digest did not bind canonical descriptor IDs and runtime digests");

		auto duplicate_descriptor =
			make_merge_descriptor("company.test.binding_duplicate", merge_mode::set);
		cxxlens::sdk::relation_registry duplicate_registry;
		require(duplicate_registry.add(duplicate_descriptor).has_value(),
				"duplicate descriptor setup failed");
		auto duplicate = duplicate_registry.add(std::move(duplicate_descriptor));
		require(!duplicate && duplicate.error().code == "sdk.duplicate-descriptor",
				"duplicate canonical descriptor ID was accepted");
		auto prefix_descriptor = make_merge_descriptor("company.a", merge_mode::set);
		auto nested_descriptor = make_merge_descriptor("company.a.b", merge_mode::set);
		const auto expected_id_order = cxxlens::sdk::semantic_digest(
			"cxxlens.relation-registry.v1",
			nested_descriptor.id + "=" + nested_descriptor.descriptor_digest + "\n" +
				prefix_descriptor.id + "=" + prefix_descriptor.descriptor_digest + "\n");
		const auto forbidden_name_order = cxxlens::sdk::semantic_digest(
			"cxxlens.relation-registry.v1",
			prefix_descriptor.name + "=" + prefix_descriptor.descriptor_digest + "\n" +
				nested_descriptor.name + "=" + nested_descriptor.descriptor_digest + "\n");
		cxxlens::sdk::relation_registry ordering_registry;
		require(ordering_registry.add(std::move(prefix_descriptor)) &&
					ordering_registry.add(std::move(nested_descriptor)),
				"descriptor ID/name order inversion setup failed");
		auto ordering_engine = ordering_registry.build("binding-generation");
		require(expected_id_order && forbidden_name_order && ordering_engine &&
					ordering_engine->registry_digest() == *expected_id_order &&
					ordering_engine->registry_digest() != *forbidden_name_order,
				"registry digest followed relation-name order instead of descriptor-ID order");
	}

	void check_descriptor_canonical_collections()
	{
		using namespace cxxlens::sdk;
		const auto rebind = [](relation_descriptor& descriptor)
		{
			descriptor.descriptor_digest =
				*semantic_digest("cxxlens.relation-descriptor-binding.v2",
								 descriptor.contract_digest + "\n" + descriptor.canonical_form());
		};

		const auto generated = cxxlens::cc::relations::call_site::descriptor();
		auto generated_permutation = generated;
		std::ranges::reverse(generated_permutation.references);
		std::ranges::reverse(generated_permutation.conflict_columns);
		require(generated_permutation.canonical_form() == generated.canonical_form() &&
					generated_permutation.descriptor_digest == generated.descriptor_digest &&
					generated_permutation.validate(),
				"generated descriptor digest depended on set insertion order");

		auto baseline = generated;
		baseline.contract_canonical.clear();
		baseline.contract_digest.clear();
		const std::vector<std::string> paired_source{baseline.columns[1].id,
													 baseline.columns[2].id};
		baseline.references = {
			{paired_source,
			 "company.target",
			 {"company.target.v1.a", "company.target.v1.b"},
			 reference_strength::hard},
			{paired_source,
			 "company.target",
			 {"company.target.v1.b", "company.target.v1.a"},
			 reference_strength::hard},
			{paired_source,
			 "company.target",
			 {"company.target.v1.a", "company.target.v1.b"},
			 reference_strength::soft_semantic},
		};
		rebind(baseline);
		require(baseline.validate().has_value(),
				"canonical collection baseline descriptor was rejected");
		const auto expected_form = baseline.canonical_form();
		const auto expected_digest = baseline.descriptor_digest;
		require(
			expected_form.contains(
				R"("strength":"hard","target_relation":"company.target","target":["company.target.v1.a","company.target.v1.b"])"),
			"reference canonicalization did not preserve source-target position pairing");

		std::array<std::size_t, 3U> reference_order{0U, 1U, 2U};
		do
		{
			auto candidate = baseline;
			candidate.references.clear();
			for (const auto index : reference_order)
				candidate.references.push_back(baseline.references[index]);
			rebind(candidate);
			require(candidate.validate() && candidate.canonical_form() == expected_form &&
						candidate.descriptor_digest == expected_digest,
					"reference permutation changed descriptor canonical identity");
		} while (std::ranges::next_permutation(reference_order).found);

		auto conflict_order = baseline.conflict_columns;
		std::ranges::sort(conflict_order);
		do
		{
			auto candidate = baseline;
			candidate.conflict_columns = conflict_order;
			rebind(candidate);
			require(candidate.validate() && candidate.canonical_form() == expected_form &&
						candidate.descriptor_digest == expected_digest,
					"conflict column permutation changed descriptor canonical identity");
		} while (std::ranges::next_permutation(conflict_order).found);
	}

	void check_claim_batch_digest_framing()
	{
		using namespace cxxlens::sdk;
		const std::span<const claim> no_claims;
		const std::span<const unresolved_reference> no_unresolved;
		const std::span<const claim_conflict> no_conflicts;
		const std::span<const differential_disagreement> no_differential;
		const auto differential_digest = [&](const differential_disagreement& value)
		{
			const std::array values{value};
			return claim_batch_content_digest(no_claims, no_unresolved, no_conflicts, values);
		};

		const std::array delimiters{std::string{":"},
									std::string{"->"},
									std::string{"|"},
									std::string{"\n"},
									std::string{"\0", 1U}};
		using differential_field = std::string differential_disagreement::*;
		const std::array<differential_field, 6U> differential_fields{
			&differential_disagreement::relation,
			&differential_disagreement::semantic_key,
			&differential_disagreement::left_interpretation,
			&differential_disagreement::right_interpretation,
			&differential_disagreement::left_content,
			&differential_disagreement::right_content,
		};
		for (const auto& delimiter : delimiters)
			for (std::size_t field{}; field + 1U < differential_fields.size(); ++field)
			{
				differential_disagreement left{"relation",
											   "semantic-key",
											   "left",
											   "right",
											   "left-content",
											   "right-content",
											   {"condition"}};
				auto right = left;
				left.*differential_fields[field] = "boundary-left" + delimiter;
				left.*differential_fields[field + 1U] = "boundary-right";
				right.*differential_fields[field] = "boundary-left";
				right.*differential_fields[field + 1U] = delimiter + "boundary-right";
				auto left_digest = differential_digest(left);
				auto right_digest = differential_digest(right);
				require(left_digest && right_digest && *left_digest != *right_digest,
						"differential disagreement field boundary was ambiguous");
			}

		differential_disagreement one_fragment{
			"relation", "key", "left", "right", "content-a", "content-b", {"a\ncondition:b"}};
		auto two_fragments = one_fragment;
		two_fragments.overlap_fragments = {"a", "b"};
		require(*differential_digest(one_fragment) != *differential_digest(two_fragments),
				"differential fragment count was not bound");

		const std::array unresolved_a{
			unresolved_reference{"assertion->target", "source", "relation", {"column"}, "reason"}};
		const std::array unresolved_b{
			unresolved_reference{"assertion", "source", "target->relation", {"column"}, "reason"}};
		auto unresolved_digest_a =
			claim_batch_content_digest(no_claims, unresolved_a, no_conflicts, no_differential);
		auto unresolved_digest_b =
			claim_batch_content_digest(no_claims, unresolved_b, no_conflicts, no_differential);
		require(unresolved_digest_a && unresolved_digest_b &&
					*unresolved_digest_a != *unresolved_digest_b,
				"unresolved record boundary or retained fields were not bound");
		for (const auto& mutation : {
				 unresolved_reference{
					 "assertion->target", "different-source", "relation", {"column"}, "reason"},
				 unresolved_reference{
					 "assertion->target", "source", "relation", {"different-column"}, "reason"},
				 unresolved_reference{
					 "assertion->target", "source", "relation", {"column"}, "different-reason"},
			 })
		{
			const std::array values{mutation};
			auto digest =
				claim_batch_content_digest(no_claims, values, no_conflicts, no_differential);
			require(digest && *digest != *unresolved_digest_a,
					"unresolved nested field was omitted from the batch digest");
		}

		const std::array conflict_a{claim_conflict{
			"relation:key", "semantic", "interpretation", {"a"}, {"assertion"}, {"content"}}};
		const std::array conflict_b{claim_conflict{
			"relation", "key:semantic", "interpretation", {"a"}, {"assertion"}, {"content"}}};
		auto conflict_digest_a =
			claim_batch_content_digest(no_claims, no_unresolved, conflict_a, no_differential);
		auto conflict_digest_b =
			claim_batch_content_digest(no_claims, no_unresolved, conflict_b, no_differential);
		require(conflict_digest_a && conflict_digest_b && *conflict_digest_a != *conflict_digest_b,
				"conflict record boundary was ambiguous");
		auto assertion_mutation = conflict_a;
		assertion_mutation.front().assertions = {"different-assertion"};
		require(*claim_batch_content_digest(
					no_claims, no_unresolved, assertion_mutation, no_differential) !=
					*conflict_digest_a,
				"conflict assertions were omitted from the batch digest");
		for (const auto& mutation : {
				 claim_conflict{"relation:key",
								"semantic",
								"different-interpretation",
								{"a"},
								{"assertion"},
								{"content"}},
				 claim_conflict{"relation:key",
								"semantic",
								"interpretation",
								{"different-fragment"},
								{"assertion"},
								{"content"}},
				 claim_conflict{"relation:key",
								"semantic",
								"interpretation",
								{"a"},
								{"assertion"},
								{"different-content"}},
			 })
		{
			const std::array values{mutation};
			auto digest =
				claim_batch_content_digest(no_claims, no_unresolved, values, no_differential);
			require(digest && *digest != *conflict_digest_a,
					"conflict nested field was omitted from the batch digest");
		}

		claim claim_a;
		claim_a.descriptor = "descriptor|content";
		claim_a.content = "claim-a";
		claim_a.interpretation = "interpretation";
		claim_a.producer = {"producer", "contract"};
		claim_a.input_basis = direct_claim_basis{"basis"};
		claim_a.guarantee = {"exact", "scope", "assumptions", {"verified"}};
		auto claim_b = claim_a;
		claim_b.descriptor = "descriptor";
		claim_b.content = "content|claim-a";
		const std::array claims_a{claim_a};
		const std::array claims_b{claim_b};
		auto claim_digest_a =
			claim_batch_content_digest(claims_a, no_unresolved, no_conflicts, no_differential);
		auto claim_digest_b =
			claim_batch_content_digest(claims_b, no_unresolved, no_conflicts, no_differential);
		require(claim_digest_a && claim_digest_b && *claim_digest_a != *claim_digest_b,
				"claim content/occurrence boundary was ambiguous");

		std::array differentials{
			differential_disagreement{"relation", "key-a", "left", "right", "a", "b", {"debug"}},
			differential_disagreement{"relation", "key-b", "left", "right", "a", "c", {"release"}},
		};
		std::array claims{claim_a, claim_b};
		std::array unresolved{
			unresolved_reference{"assertion-a", "source-a", "target-a", {"column-a"}, "soft"},
			unresolved_reference{"assertion-b", "source-b", "target-b", {"column-b"}, "soft"},
		};
		std::array conflicts{conflict_a.front(), conflict_b.front()};
		auto reversed_claims = claims;
		auto reversed_unresolved = unresolved;
		auto reversed_conflicts = conflicts;
		auto reversed_differentials = differentials;
		std::ranges::reverse(reversed_claims);
		std::ranges::reverse(reversed_unresolved);
		std::ranges::reverse(reversed_conflicts);
		std::ranges::reverse(reversed_differentials);
		auto forward = claim_batch_content_digest(claims, unresolved, conflicts, differentials);
		auto reversed = claim_batch_content_digest(
			reversed_claims, reversed_unresolved, reversed_conflicts, reversed_differentials);
		require(forward &&
					*forward ==
						"semantic-v2:sha256:"
						"202750ee0bf84e826f6ebb4b739a054f82449c52326829c6c311b3422d0f6140",
				"claim batch v2 golden digest changed");
		require(forward && reversed && *forward == *reversed,
				"set-like batch record permutation changed the digest");
		auto encoded = claim_batch_content_encoding(claims, unresolved, conflicts, differentials);
		auto decoded =
			encoded ? canonical_binary_decode(*encoded) : result<canonical_value>{encoded.error()};
		auto reencoded =
			decoded ? canonical_binary(*decoded) : result<std::vector<std::byte>>{decoded.error()};
		require(encoded && decoded && reencoded && *encoded == *reencoded,
				"claim batch encode/decode/encode changed canonical bytes");
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

	void check_static_row_view_validation()
	{
		using call = cxxlens::cc::relations::call_site;
		auto valid_call = make_call_row();
		call::view valid_view{valid_call};
		auto static_ordinal = valid_view.get<call::ordinal>();
		auto optional_caller = valid_view.get<call::caller>();
		require(static_ordinal &&
					static_ordinal->canonical_form() ==
						valid_call.cells.at(call::ordinal::ref().column_id).canonical_form() &&
					optional_caller && optional_caller->state == cxxlens::sdk::cell_state::absent &&
					optional_caller->type == call::caller::ref().type,
				"validated dynamic/static read parity or typed optional absence failed");

		auto wrong_type = valid_call;
		wrong_type.cells.at(call::ordinal::ref().column_id) =
			cxxlens::sdk::detached_cell::utf8("not-an-integer");
		auto wrong_type_result = call::view{wrong_type}.get<call::ordinal>();
		require(!wrong_type_result && wrong_type_result.error().code == "sdk.cell-type-mismatch" &&
					wrong_type_result.error().field == call::ordinal::ref().column_id,
				"static typed read accepted a wrong-type integer cell");

		auto invalid_utf8 = valid_call;
		invalid_utf8.cells.at(call::source::ref().column_id) =
			cxxlens::sdk::detached_cell::typed("source_span_id", std::string{"bad\xff", 4U});
		auto invalid_utf8_result = call::view{invalid_utf8}.get<call::source>();
		require(!invalid_utf8_result && invalid_utf8_result.error().code == "sdk.cell-invalid",
				"static typed read accepted invalid UTF-8");

		auto different_minor_shape = valid_call;
		different_minor_shape.cells.emplace(call::descriptor().id + ".future_optional",
											cxxlens::sdk::detached_cell::utf8("future"));
		auto different_shape_result = call::view{different_minor_shape}.get<call::ordinal>();
		require(!different_shape_result &&
					different_shape_result.error().code == "sdk.unknown-cell",
				"static typed read accepted a row with a different descriptor shape");

		auto foreign_column = valid_view.get<cxxlens::cc::relations::entity::entity_column>();
		require(!foreign_column && foreign_column.error().code == "sdk.foreign-column",
				"static typed read accepted a foreign Column tag");

		using entity = cxxlens::cc::relations::entity;
		entity::builder entity_builder;
		for (auto result : {
				 entity_builder.set<entity::entity_column>(
					 cxxlens::sdk::detached_cell::typed("cc_entity_id", "entity:typed-view")),
				 entity_builder.set<entity::canonicalization>(
					 string_cell({cxxlens::sdk::scalar_kind::closed_symbol,
								  "cc.canonicalization-state/1",
								  false},
								 "canonicalized")),
				 entity_builder.set<entity::kind>(string_cell(
					 {cxxlens::sdk::scalar_kind::open_symbol, "cc.entity-kind/1", false},
					 "function")),
				 entity_builder.set<entity::structural_signature_digest>(
					 string_cell({cxxlens::sdk::scalar_kind::digest, {}, false}, digest('a'))),
			 })
			require(result.has_value(), "static view entity fixture cell rejected");
		auto valid_entity = std::move(entity_builder).finish();
		require(valid_entity.has_value(), "static view entity fixture row rejected");

		auto invalid_digest = *valid_entity;
		invalid_digest.cells.at(entity::structural_signature_digest::ref().column_id) =
			string_cell({cxxlens::sdk::scalar_kind::digest, {}, false}, "sha256:not-canonical");
		auto invalid_digest_result =
			entity::view{invalid_digest}.get<entity::structural_signature_digest>();
		require(!invalid_digest_result && invalid_digest_result.error().code == "sdk.cell-invalid",
				"static typed read accepted an invalid digest");

		auto invalid_closed_symbol = *valid_entity;
		invalid_closed_symbol.cells.at(entity::canonicalization::ref().column_id) = string_cell(
			{cxxlens::sdk::scalar_kind::closed_symbol, "cc.canonicalization-state/1", false},
			"forged-state");
		auto invalid_closed_result =
			entity::view{invalid_closed_symbol}.get<entity::canonicalization>();
		require(!invalid_closed_result && invalid_closed_result.error().code == "sdk.cell-invalid",
				"static typed read accepted an invalid closed symbol");
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
		require(decoded && decoded->type == frame.type && decoded->protocol_major == 1U &&
					decoded->protocol_minor == 0U && decoded->flags == 0U,
				"protocol frame header did not round-trip");
		std::string valid_control{"ASCII|"};
		valid_control += "\xe2\x82\xac|";
		valid_control += "\xf0\x9f\x98\x80|";
		valid_control.push_back('\0');
		auto encoded_control = cxxlens::sdk::provider::encode_control_text(valid_control);
		require(encoded_control.has_value(), "valid UTF-8 CBOR text encoding failed");
		auto decoded_control = cxxlens::sdk::provider::decode_control_text(*encoded_control);
		require(decoded_control && *decoded_control == valid_control,
				"valid ASCII/BMP/non-BMP/NUL CBOR text did not round-trip");
		for (const auto invalid : std::array{
				 std::string_view{"\x80", 1U},
				 std::string_view{"\xc2", 1U},
				 std::string_view{"\xc0\x80", 2U},
				 std::string_view{"\xe0\x80\x80", 3U},
				 std::string_view{"\xf0\x80\x80\x80", 4U},
				 std::string_view{"\xed\xa0\x80", 3U},
				 std::string_view{"\xf4\x90\x80\x80", 4U},
			 })
		{
			std::vector<std::byte> invalid_control{static_cast<std::byte>(0x60U | invalid.size())};
			for (const auto byte : invalid)
				invalid_control.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
			auto rejected_decode = cxxlens::sdk::provider::decode_control_text(invalid_control);
			auto rejected_encode = cxxlens::sdk::provider::encode_control_text(invalid);
			require(
				!rejected_decode && rejected_decode.error().code == "provider.malformed-frame" &&
					!rejected_encode && rejected_encode.error().code == "provider.malformed-frame",
				"invalid UTF-8 CBOR control text was accepted");
		}
		auto future_minor = *encoded;
		future_minor.at(7U) = std::byte{0x01};
		auto unsupported_minor = cxxlens::sdk::provider::decode_frame(future_minor);
		require(!unsupported_minor &&
					unsupported_minor.error().code == "provider.protocol-minor-mismatch",
				"unsupported protocol minor was accepted");
		cxxlens::sdk::provider::protocol_limits compatible_minor;
		compatible_minor.maximum_minor = 1U;
		auto negotiated_minor =
			cxxlens::sdk::provider::decode_frame(future_minor, compatible_minor);
		require(negotiated_minor && negotiated_minor->protocol_minor == 1U,
				"negotiated compatible protocol minor was rejected");
		auto required_extension = *encoded;
		required_extension.at(11U) = std::byte{0x11};
		auto unknown_required = cxxlens::sdk::provider::decode_frame(required_extension);
		require(!unknown_required &&
					unknown_required.error().code == "provider.unknown-required-extension",
				"unknown required extension flag was accepted");
		auto optional_extension = *encoded;
		optional_extension.at(8U) = std::byte{0xfd};
		optional_extension.at(9U) = std::byte{0xe8};
		optional_extension.at(11U) = std::byte{0x02};
		auto unknown_optional = cxxlens::sdk::provider::decode_frame(optional_extension);
		require(unknown_optional && unknown_optional->flags == 2U &&
					static_cast<std::uint16_t>(unknown_optional->type) == 65000U,
				"unknown optional extension was not preserved for skip accounting");
		auto compressed = *encoded;
		compressed.at(11U) = std::byte{0x04};
		auto unsupported_compression = cxxlens::sdk::provider::decode_frame(compressed);
		require(!unsupported_compression &&
					unsupported_compression.error().code == "provider.unsupported-compression",
				"compressed payload without a negotiated codec was accepted");
		auto reserved = *encoded;
		reserved.at(11U) = std::byte{0x10};
		auto unknown_flags = cxxlens::sdk::provider::decode_frame(reserved);
		require(!unknown_flags && unknown_flags.error().code == "provider.invalid-frame-flags",
				"reserved frame flag was accepted");
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

	void check_columnar_wire_codec()
	{
		using namespace cxxlens::sdk;
		using namespace cxxlens::sdk::provider;
		const std::string descriptor_digest =
			"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
		const std::array columns{
			column_descriptor{"company.test.columnar.v1.bool",
							  "bool",
							  {scalar_kind::boolean, {}, false},
							  true,
							  column_role::authoritative_payload},
			column_descriptor{"company.test.columnar.v1.int",
							  "int",
							  {scalar_kind::signed_integer, {}, false},
							  true,
							  column_role::authoritative_payload},
			column_descriptor{"company.test.columnar.v1.text",
							  "text",
							  {scalar_kind::utf8_string, {}, true},
							  false,
							  column_role::authoritative_payload},
			column_descriptor{"company.test.columnar.v1.bytes",
							  "bytes",
							  {scalar_kind::bytes, {}, false},
							  true,
							  column_role::authoritative_payload},
			column_descriptor{"company.test.columnar.v1.unknown",
							  "unknown",
							  {scalar_kind::open_symbol, "company.test.symbol/1", false},
							  true,
							  column_role::authoritative_payload},
		};
		const std::array<std::vector<detached_cell>, columns.size()> cells{
			std::vector{detached_cell::boolean(true), detached_cell::boolean(false)},
			std::vector{detached_cell::signed_integer(-42), detached_cell::signed_integer(7)},
			std::vector{string_cell(columns[2U].type, "hello"),
						detached_cell::absent(columns[2U].type)},
			std::vector{detached_cell::bytes({std::byte{}, std::byte{0xff}}),
						detached_cell::bytes({std::byte{1U}})},
			std::vector{detached_cell::unknown(columns[4U].type, "not-observed"),
						string_cell(columns[4U].type, "known")},
		};
		const std::array encodings{"fixed-width-bool-u8",
								   "fixed-width-i64-le",
								   "utf8-offsets-u32-le",
								   "bytes-offsets-u32-le",
								   "dictionary-index-u32-le"};
		std::vector<batch_column_summary> summaries;
		std::vector<std::string> digests;
		std::vector<encoded_column_chunk> encoded_chunks;
		for (std::size_t index = 0U; index < columns.size(); ++index)
		{
			column_chunk_record chunk{"task-columnar",
									  "dependency-1",
									  "atomic-1",
									  "batch-1",
									  "company.test.columnar.v1",
									  descriptor_digest,
									  columns[index].id,
									  10U,
									  2U,
									  3U,
									  encodings[index],
									  cells[index],
									  {}};
			auto encoded = encode_column_chunk(chunk, columns[index]);
			require(encoded.has_value(),
					"typed column chunk did not encode: " + std::to_string(index) + " " +
						(encoded ? std::string{}
								 : encoded.error().code + "/" + encoded.error().detail));
			auto decoded = decode_column_chunk(encoded->control, encoded->payload, columns[index]);
			require(decoded && decoded->task_id == chunk.task_id &&
						decoded->dependency_group_id == chunk.dependency_group_id &&
						decoded->atomic_output_group_id == chunk.atomic_output_group_id &&
						decoded->batch_id == chunk.batch_id &&
						decoded->descriptor_id == chunk.descriptor_id &&
						decoded->descriptor_digest == chunk.descriptor_digest &&
						decoded->column_id == chunk.column_id && decoded->row_offset == 10U &&
						decoded->row_count == 2U && decoded->chunk_index == 3U &&
						decoded->encoding == chunk.encoding &&
						decoded->cells[0U].canonical_form() == cells[index][0U].canonical_form() &&
						decoded->cells[1U].canonical_form() == cells[index][1U].canonical_form() &&
						decoded->chunk_digest == encoded->chunk_digest,
					"typed column chunk roundtrip lost binding or cell state");
			summaries.push_back({columns[index].id, encoded->payload.size(), 1U});
			digests.push_back(encoded->chunk_digest);
			encoded_chunks.push_back(std::move(*encoded));
		}
		const std::array reference_bool_payload{
			std::byte{0x43}, std::byte{0x58}, std::byte{0x43}, std::byte{0x43}, std::byte{0x01},
			std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
			std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
			std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
			std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0c},
			std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
			std::byte{0x00}, std::byte{0x00}, std::byte{0x03}, std::byte{0x00}, std::byte{0x01},
			std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
			std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
			std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
		};
		require(std::ranges::equal(encoded_chunks.front().payload, reference_bool_payload),
				"C++ column payload diverged from the independent reference vector");

		auto malformed_validity = encoded_chunks.front();
		malformed_validity.payload[32U] |= std::byte{0x80};
		auto validity_rejected = decode_column_chunk(
			malformed_validity.control, malformed_validity.payload, columns.front());
		require(!validity_rejected && validity_rejected.error().detail == "unused-validity-bits",
				"non-zero unused validity bits were accepted");
		auto malformed_offset = encoded_chunks[2U];
		malformed_offset.payload[34U] ^= std::byte{1U};
		auto offset_rejected =
			decode_column_chunk(malformed_offset.control, malformed_offset.payload, columns[2U]);
		require(!offset_rejected && offset_rejected.error().detail == "offsets",
				"malformed variable-width offsets were accepted");
		auto malformed_dictionary = encoded_chunks[4U];
		const auto auxiliary_size = static_cast<std::size_t>(
			std::to_integer<std::uint8_t>(malformed_dictionary.payload[16U]));
		const auto dictionary_values = 34U + auxiliary_size;
		malformed_dictionary.payload[dictionary_values + 4U] = std::byte{0xff};
		auto dictionary_rejected = decode_column_chunk(
			malformed_dictionary.control, malformed_dictionary.payload, columns[4U]);
		require(!dictionary_rejected && dictionary_rejected.error().detail == "dictionary-index",
				"out-of-range dictionary index was accepted");

		columnar_batch_end terminal{"task-columnar",
									"dependency-1",
									"atomic-1",
									"batch-1",
									"company.test.columnar.v1",
									descriptor_digest,
									12U,
									std::move(summaries),
									std::move(digests),
									{}};
		terminal.batch_digest = columnar_batch_digest(terminal);
		auto encoded_terminal = encode_columnar_batch_end(terminal);
		require(encoded_terminal.has_value(), "columnar batch summary did not encode");
		auto decoded_terminal =
			decode_columnar_batch_end(encoded_terminal->control, encoded_terminal->payload);
		require(decoded_terminal && decoded_terminal->columns == terminal.columns &&
					decoded_terminal->ordered_chunk_digests == terminal.ordered_chunk_digests &&
					decoded_terminal->batch_digest == terminal.batch_digest,
				"column lengths/order/digests did not survive batch-end roundtrip");

		const auto collision_chunk = [&](std::string task, std::string dependency)
		{
			return column_chunk_record{std::move(task),
									   std::move(dependency),
									   "atomic-1",
									   "batch-collision",
									   "company.test.columnar.v1",
									   descriptor_digest,
									   columns.front().id,
									   0U,
									   2U,
									   0U,
									   "fixed-width-bool-u8",
									   cells.front(),
									   {}};
		};
		const std::array boundary_prefixes{
			std::string{"task"}, std::string{"task|pipe"}, std::string{"task\0nul", 8U}};
		const std::array chunk_text_fields{&column_chunk_record::task_id,
										   &column_chunk_record::dependency_group_id,
										   &column_chunk_record::atomic_output_group_id,
										   &column_chunk_record::batch_id,
										   &column_chunk_record::descriptor_id};
		for (std::size_t field_index = 0U; field_index + 1U < chunk_text_fields.size();
			 ++field_index)
			for (const auto& prefix : boundary_prefixes)
			{
				auto left = collision_chunk("task", "dep");
				auto right = left;
				left.*chunk_text_fields[field_index] = prefix;
				left.*chunk_text_fields[field_index + 1U] = "right\nextra";
				right.*chunk_text_fields[field_index] = prefix + "\nright";
				right.*chunk_text_fields[field_index + 1U] = "extra";
				auto encoded_left = encode_column_chunk(left, columns.front());
				auto encoded_right = encode_column_chunk(right, columns.front());
				require(encoded_left && encoded_right &&
							encoded_left->chunk_digest != encoded_right->chunk_digest &&
							encoded_left->control != encoded_right->control,
						"typed chunk digest collapsed a delimiter boundary shift");
				auto decoded_left = decode_column_chunk(
					encoded_left->control, encoded_left->payload, columns.front());
				auto decoded_right = decode_column_chunk(
					encoded_right->control, encoded_right->payload, columns.front());
				require(decoded_left && decoded_right && decoded_left->task_id == left.task_id &&
							decoded_left->dependency_group_id == left.dependency_group_id &&
							decoded_right->task_id == right.task_id &&
							decoded_right->dependency_group_id == right.dependency_group_id &&
							(*decoded_left).*chunk_text_fields[field_index] ==
								left.*chunk_text_fields[field_index] &&
							(*decoded_left).*chunk_text_fields[field_index + 1U] ==
								left.*chunk_text_fields[field_index + 1U] &&
							(*decoded_right).*chunk_text_fields[field_index] ==
								right.*chunk_text_fields[field_index] &&
							(*decoded_right).*chunk_text_fields[field_index + 1U] ==
								right.*chunk_text_fields[field_index + 1U],
						"chunk control fields were not exactly bound to the typed digest");
			}

		auto batch_left = terminal;
		auto batch_right = terminal;
		const std::array batch_text_fields{&columnar_batch_end::task_id,
										   &columnar_batch_end::dependency_group_id,
										   &columnar_batch_end::atomic_output_group_id,
										   &columnar_batch_end::batch_id,
										   &columnar_batch_end::descriptor_id};
		for (std::size_t field_index = 0U; field_index + 1U < batch_text_fields.size();
			 ++field_index)
			for (const auto& prefix : boundary_prefixes)
			{
				batch_left = terminal;
				batch_right = terminal;
				batch_left.*batch_text_fields[field_index] = prefix;
				batch_left.*batch_text_fields[field_index + 1U] = "right\nextra";
				batch_right.*batch_text_fields[field_index] = prefix + "\nright";
				batch_right.*batch_text_fields[field_index + 1U] = "extra";
				require(columnar_batch_digest(batch_left) != columnar_batch_digest(batch_right),
						"typed batch digest collapsed a text field boundary shift");
			}
		batch_left = terminal;
		batch_left.task_id = "task";
		batch_left.dependency_group_id = "dep\nextra";
		batch_left.batch_digest = columnar_batch_digest(batch_left);
		auto encoded_batch_left = encode_columnar_batch_end(batch_left);
		require(encoded_batch_left.has_value(), "delimiter-bearing batch did not encode");
		auto decoded_batch_left =
			decode_columnar_batch_end(encoded_batch_left->control, encoded_batch_left->payload);
		require(decoded_batch_left && decoded_batch_left->task_id == batch_left.task_id &&
					decoded_batch_left->dependency_group_id == batch_left.dependency_group_id &&
					decoded_batch_left->batch_digest == batch_left.batch_digest,
				"batch control fields were not exactly bound to the typed digest");
		auto list_left = terminal;
		auto list_right = terminal;
		list_left.ordered_chunk_digests = {"a\nb", "c"};
		list_right.ordered_chunk_digests = {"a", "b\nc"};
		require(columnar_batch_digest(list_left) != columnar_batch_digest(list_right),
				"typed batch digest collapsed ordered collection element boundaries");
		auto permuted = terminal;
		std::ranges::swap(permuted.columns.front(), permuted.columns.back());
		std::ranges::swap(permuted.ordered_chunk_digests.front(),
						  permuted.ordered_chunk_digests.back());
		require(columnar_batch_digest(permuted) != terminal.batch_digest,
				"ordered column or chunk permutation did not change the batch digest");
	}

	void check_relation_sink_batch_state()
	{
		using namespace cxxlens::sdk;
		using namespace cxxlens::sdk::provider;
		class fault_sink final : public frame_sink
		{
		  public:
			result<void> write(const std::span<const std::byte> bytes) override
			{
				const auto attempt = write_attempts++;
				if (fail_at && attempt == *fail_at)
					return cxxlens::sdk::unexpected(
						cxxlens::sdk::error{"provider.backpressure", "test-sink", {}});
				transcript.insert(transcript.end(), bytes.begin(), bytes.end());
				return {};
			}

			std::optional<std::size_t> fail_at;
			std::size_t write_attempts{};
			std::vector<std::byte> transcript;
		};

		const auto descriptor = cxxlens::cc::relations::call_site::descriptor();
		const std::array outputs{descriptor};
		const std::array dependencies{std::string{"dependency-1"}};
		fault_sink sink;
		protocol_writer writer{sink};
		writer.grant_credit({64U * 1024U * 1024U, 65536U});
		execution_context execution;
		execution.budget.rows = 2U;
		context provider_context{writer, execution, "task:batch-reset", outputs, dependencies};
		auto output = provider_context.relation(descriptor);

		auto empty_begin = output.begin("dependency-1", "atomic-1", "empty-batch");
		require(empty_begin.has_value(),
				"empty first batch did not begin: " +
					(empty_begin ? std::string{} : empty_begin.error().code));
		require(output.row_count() == 0U, "empty first batch began with a nonzero row count");
		auto empty_end = output.end();
		require(empty_end.has_value(),
				"empty first batch did not end: " +
					(empty_end ? std::string{}
							   : empty_end.error().code + "/" + empty_end.error().detail));
		for (const auto* batch : {"batch-1", "batch-2"})
		{
			require(output.begin("dependency-1", "atomic-1", batch).has_value() &&
						output.row_count() == 0U,
					"begin did not reset the batch-local row count");
			require(output.push(make_call_row()).has_value() && output.row_count() == 1U &&
						output.end().has_value() && output.row_count() == 1U,
					"nonempty batch did not retain its own row count");
		}
		require(output.begin("dependency-1", "atomic-1", "budget-batch").has_value() &&
					output.row_count() == 0U,
				"third begin did not reset batch-local state");
		auto exhausted = output.push(make_call_row());
		require(!exhausted && exhausted.error().code == "provider.output-limit" &&
					output.row_count() == 0U,
				"task-global row budget was reset with batch-local state");

		auto frames = decode_frame_stream(sink.transcript);
		require(frames.has_value(), "multi-batch transcript did not decode");
		std::size_t empty_terminals{};
		std::size_t first_terminals{};
		std::size_t second_terminals{};
		std::size_t second_chunks{};
		for (const auto& frame : *frames)
		{
			if (frame.type == message_type::column_chunk)
			{
				auto chunk = decode_column_chunk(frame.control, frame.payload, descriptor);
				require(chunk.has_value(), "multi-batch column chunk did not decode");
				if (chunk->batch_id == "batch-2")
				{
					++second_chunks;
					require(chunk->row_offset == 0U && chunk->row_count == 1U &&
								chunk->chunk_index == 0U,
							"second batch inherited row offset or chunk index");
				}
			}
			if (frame.type == message_type::batch_end)
			{
				auto terminal = decode_columnar_batch_end(frame.control, frame.payload);
				require(terminal.has_value(), "multi-batch terminal digest did not validate");
				if (terminal->batch_id == "empty-batch")
					empty_terminals += terminal->row_count == 0U ? 1U : 100U;
				else if (terminal->batch_id == "batch-1")
					first_terminals += terminal->row_count == 1U ? 1U : 100U;
				else if (terminal->batch_id == "batch-2")
					second_terminals += terminal->row_count == 1U ? 1U : 100U;
			}
		}
		require(empty_terminals == 1U && first_terminals == 1U && second_terminals == 1U &&
					second_chunks == descriptor.columns.size(),
				"multi-batch row counts, column coverage, or batch digests diverged");

		fault_sink failing_sink;
		failing_sink.fail_at = 2U;
		protocol_writer failing_writer{failing_sink};
		failing_writer.grant_credit({64U * 1024U * 1024U, 65536U});
		execution_context failing_execution;
		failing_execution.budget.rows = 10U;
		context failing_context{
			failing_writer, failing_execution, "task:partial-send", outputs, dependencies};
		auto failing_output = failing_context.relation(descriptor);
		require(failing_output.begin("dependency-1", "atomic-1", "partial-batch").has_value() &&
					failing_output.push(make_call_row()).has_value(),
				"partial-send fixture setup failed");
		auto partial = failing_output.end();
		require(!partial && partial.error().code == "provider.backpressure",
				"injected later-column send failure was not observed");
		const auto attempts_after_failure = failing_sink.write_attempts;
		for (auto rejected : {failing_output.push(make_call_row()),
							  failing_output.end(),
							  failing_output.begin("dependency-1", "atomic-2", "retry-batch")})
			require(!rejected && rejected.error().code == "provider.batch-state-invalid",
					"poisoned relation sink permitted a retry after partial column output");
		require(failing_sink.write_attempts == attempts_after_failure &&
					failing_output.row_count() == 1U,
				"poisoned sink emitted duplicate output or lost failed-batch accounting");
		auto failed_prefix = decode_frame_stream(failing_sink.transcript);
		require(failed_prefix && failed_prefix->size() == 2U &&
					failed_prefix->front().type == message_type::batch_begin &&
					failed_prefix->back().type == message_type::column_chunk,
				"partial send did not leave one sealed, non-retried transcript prefix");
	}

	void check_project_catalog_identity()
	{
		using cxxlens::sdk::canonical_value;
		const auto unit_a = catalog_unit("unit:a", '1');
		const auto unit_b = catalog_unit("unit:b", '4');
		auto ordered = make_catalog("workspace", {unit_a, unit_b});
		auto permuted = make_catalog("workspace", {unit_b, unit_a});
		auto ordered_projection = ordered.canonical_projection();
		auto permuted_projection = permuted.canonical_projection();
		require(ordered.catalog_id == permuted.catalog_id &&
					ordered.catalog_digest == permuted.catalog_digest && ordered_projection &&
					permuted_projection && *ordered_projection == *permuted_projection,
				"catalog input permutation changed canonical identity");

		auto added = ordered;
		added.compile_units.push_back(catalog_unit("unit:c", '7'));
		auto removed = ordered;
		removed.compile_units.pop_back();
		auto replaced = ordered;
		replaced.compile_units.front().source_digest = digest('9');
		auto moved_root = ordered;
		moved_root.logical_root = "relocated-workspace";
		auto malformed = ordered;
		malformed.catalog_digest = "sha256:not-a-digest";
		require(!added.validate() && !removed.validate() && !replaced.validate() &&
					!moved_root.validate() && !malformed.validate() &&
					malformed.validate().error().field == "catalog_digest",
				"catalog validation did not bind every authoritative input");

		auto duplicate =
			cxxlens::sdk::project_catalog::make("workspace", digest('d'), {unit_a, unit_a});
		auto conflicting_unit = unit_a;
		conflicting_unit.source_digest = digest('e');
		auto conflicting = cxxlens::sdk::project_catalog::make(
			"workspace", digest('d'), {unit_a, conflicting_unit});
		require(!duplicate && !conflicting && duplicate.error().detail == "duplicate-or-conflict" &&
					conflicting.error().detail == "duplicate-or-conflict",
				"duplicate or conflicting compile-unit identity was accepted");

		std::vector<canonical_value> independent_units;
		for (const auto& unit : ordered.compile_units)
			independent_units.push_back(canonical_value::from_tuple({
				canonical_value::from_string(unit.compile_unit_id),
				canonical_value::from_string(unit.effective_invocation_digest),
				canonical_value::from_string(unit.source_digest),
				canonical_value::from_string(unit.environment_digest),
			}));
		auto independent = cxxlens::sdk::canonical_binary(canonical_value::from_tuple({
			canonical_value::from_string("cxxlens.project-catalog.v1"),
			canonical_value::from_string(ordered.logical_root),
			canonical_value::from_string(ordered.environment_digest),
			canonical_value::from_tuple(std::move(independent_units)),
		}));
		auto projection = ordered.canonical_projection();
		require(projection && independent && *independent == *projection,
				"independent catalog encoder was not byte-identical");

		using relation = cxxlens::build::relations::project;
		cxxlens::sdk::detached_row project_row{relation::descriptor().id, {}};
		project_row.cells.emplace(
			relation::catalog::ref().column_id,
			cxxlens::sdk::detached_cell::typed("catalog_id", ordered.catalog_id));
		project_row.cells.emplace(
			relation::catalog_digest::ref().column_id,
			string_cell({cxxlens::sdk::scalar_kind::digest, {}, false}, ordered.catalog_digest));
		project_row.cells.emplace(
			relation::logical_root::ref().column_id,
			cxxlens::sdk::detached_cell::typed("logical_path_id", ordered.logical_root));
		project_row.cells.emplace(relation::environment_digest::ref().column_id,
								  string_cell({cxxlens::sdk::scalar_kind::digest, {}, false},
											  ordered.environment_digest));
		project_row.cells.emplace(
			relation::project_column::ref().column_id,
			cxxlens::sdk::detached_cell::typed("project_id", "project:pending"));
		auto project_id = cxxlens::sdk::derive_domain_identity(relation::descriptor(), project_row);
		require(project_id.has_value(), "build.project identity derivation failed");
		project_row.cells.at(relation::project_column::ref().column_id) =
			cxxlens::sdk::detached_cell::typed("project_id", std::move(*project_id));
		require(cxxlens::sdk::validate_row(relation::descriptor(), project_row) &&
					cxxlens::sdk::validate_domain_identity(relation::descriptor(), project_row),
				"build.project row did not preserve catalog identity fields");
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
					portable_manifest->content.contains(R"("provider_version":"1.0.0")") &&
					!portable_manifest->content.contains(R"("provider_version":"0.)") &&
					native_cmake != native->end() &&
					native_cmake->content.contains("cxxlens::clang22_provider_sdk"),
				"provider scaffold package/runtime-valid manifest contract diverged");

		coverage_provider implementation;
		auto task = make_provider_task(implementation,
									   make_catalog(".", {catalog_unit("unit-1", '1')}),
									   {cxxlens::cc::relations::call_site::descriptor()});
		cxxlens::sdk::testing::provider_harness harness;
		auto accepted = harness.run(implementation, task);
		auto corrupt = harness.run(
			implementation, task, cxxlens::sdk::testing::provider_fault::corrupt_checksum);
		auto truncated = harness.run(
			implementation, task, cxxlens::sdk::testing::provider_fault::truncate_last_frame);
		auto cancelled = harness.run(
			implementation, task, cxxlens::sdk::testing::provider_fault::cancel_before_run);
		auto wrong_direction = harness.run(
			implementation, task, cxxlens::sdk::testing::provider_fault::wrong_direction);
		auto missing_terminal =
			harness.run(implementation, task, cxxlens::sdk::testing::provider_fault::drop_terminal);
		auto wrong_terminal = harness.run(
			implementation, task, cxxlens::sdk::testing::provider_fault::wrong_terminal_task);
		auto credit_exceeded = harness.run(
			implementation, task, cxxlens::sdk::testing::provider_fault::credit_exceeded);
		auto forged_task = task;
		forged_task.project.logical_root = "forged-root";
		auto forged = harness.run(implementation, forged_task);
		auto mutated_task = task;
		mutated_task.condition = "condition:other";
		auto mutated = harness.run(implementation, mutated_task);
		auto shape_mismatch = task;
		shape_mismatch.outputs.front().columns.front().required = false;
		auto mismatched_shape = harness.run(implementation, shape_mismatch);
		const auto& output_descriptor = cxxlens::cc::relations::call_site::descriptor();
		auto duplicate_outputs =
			cxxlens::sdk::provider::task::make(task.session,
											   task.project,
											   {output_descriptor, output_descriptor},
											   task.condition,
											   task.interpretation);
		auto conflicting_descriptor = output_descriptor;
		conflicting_descriptor.descriptor_digest = digest('e');
		auto conflicting_outputs =
			cxxlens::sdk::provider::task::make(task.session,
											   task.project,
											   {output_descriptor, conflicting_descriptor},
											   task.condition,
											   task.interpretation);
		auto empty_condition = cxxlens::sdk::provider::task::make(
			task.session, task.project, task.outputs, "", task.interpretation);
		auto invalid_interpretation = cxxlens::sdk::provider::task::make(
			task.session, task.project, task.outputs, task.condition, "invalid");
		invalid_identity_provider invalid_id{false};
		invalid_identity_provider invalid_version{true};
		auto invalid_id_report = harness.run(invalid_id, task);
		auto invalid_version_report = harness.run(invalid_version, task);
		auto reference = accepted
			? cxxlens::sdk::testing::validate_logical_transcript(
				  task,
				  implementation.id(),
				  implementation.version(),
				  accepted->frames,
				  {std::numeric_limits<std::uint64_t>::max(), 4096U})
			: cxxlens::sdk::result<cxxlens::sdk::testing::conformance_report>{
				  cxxlens::sdk::unexpected(cxxlens::sdk::error{"sdk.test-setup", "accepted", {}})};
		auto invalid_reference = wrong_direction
			? cxxlens::sdk::testing::validate_logical_transcript(
				  task,
				  implementation.id(),
				  implementation.version(),
				  wrong_direction->frames,
				  {std::numeric_limits<std::uint64_t>::max(), 4096U})
			: reference;
		require(accepted && accepted->accepted && reference && reference->accepted,
				"exact provider task/session was rejected");

		using namespace cxxlens::sdk::provider;
		const auto special = structured_control_text();
		const task_accepted_metadata accepted_metadata{special, special, special};
		auto accepted_control = encode_task_accepted_metadata(accepted_metadata);
		auto accepted_round_trip = accepted_control
			? decode_task_accepted_metadata(*accepted_control)
			: cxxlens::sdk::result<task_accepted_metadata>{
				  cxxlens::sdk::unexpected(accepted_control.error())};
		const batch_begin_metadata begin_metadata{
			special, special, special, special, special, special};
		auto begin_control = encode_batch_begin_metadata(begin_metadata);
		auto begin_round_trip = begin_control
			? decode_batch_begin_metadata(*begin_control)
			: cxxlens::sdk::result<batch_begin_metadata>{
				  cxxlens::sdk::unexpected(begin_control.error())};
		const std::array coverage_values{coverage_unit{special, special, special, special}};
		const std::array unresolved_values{unresolved_item{special, special, special}};
		const std::array evidence_values{evidence_item{special, special, special, special}};
		auto coverage_control = encode_coverage_metadata(coverage_values);
		auto unresolved_control = encode_unresolved_metadata(unresolved_values);
		auto evidence_control = encode_evidence_metadata(evidence_values);
		const std::array evidence_order_a{
			evidence_item{"provider.evidence", "subject-b", "producer", "summary-b"},
			evidence_item{"provider.evidence", "subject-a", "producer", "summary-a"}};
		auto evidence_order_b = evidence_order_a;
		std::ranges::reverse(evidence_order_b);
		auto canonical_evidence_a = encode_evidence_metadata(evidence_order_a);
		auto canonical_evidence_b = encode_evidence_metadata(evidence_order_b);
		const std::array duplicate_evidence{evidence_order_a.front(), evidence_order_a.front()};
		auto rejected_duplicate_evidence = encode_evidence_metadata(duplicate_evidence);
		auto coverage_round_trip = coverage_control
			? decode_coverage_metadata(*coverage_control)
			: cxxlens::sdk::result<std::vector<coverage_unit>>{
				  cxxlens::sdk::unexpected(coverage_control.error())};
		auto unresolved_round_trip = unresolved_control
			? decode_unresolved_metadata(*unresolved_control)
			: cxxlens::sdk::result<std::vector<unresolved_item>>{
				  cxxlens::sdk::unexpected(unresolved_control.error())};
		auto evidence_round_trip = evidence_control
			? decode_evidence_metadata(*evidence_control)
			: cxxlens::sdk::result<std::vector<evidence_item>>{
				  cxxlens::sdk::unexpected(evidence_control.error())};
		const task_complete_metadata complete_metadata{special};
		const task_failed_metadata failed_metadata{special, special, special};
		auto complete_control = encode_task_complete_metadata(complete_metadata);
		auto failed_control = encode_task_failed_metadata(failed_metadata);
		auto complete_round_trip = complete_control
			? decode_task_complete_metadata(*complete_control)
			: cxxlens::sdk::result<task_complete_metadata>{
				  cxxlens::sdk::unexpected(complete_control.error())};
		auto failed_round_trip = failed_control
			? decode_task_failed_metadata(*failed_control)
			: cxxlens::sdk::result<task_failed_metadata>{
				  cxxlens::sdk::unexpected(failed_control.error())};
		const schema_negotiate_metadata schema_metadata{special,
														std::numeric_limits<std::uint64_t>::max()};
		const open_task_metadata open_metadata{special, special, special, special, special};
		const credit_metadata credit_metadata_value{std::numeric_limits<std::uint64_t>::max(), 42U};
		const close_metadata close_metadata_value{special};
		auto schema_control = encode_schema_negotiate_metadata(schema_metadata);
		auto open_control = encode_open_task_metadata(open_metadata);
		auto credit_control = encode_credit_metadata(credit_metadata_value);
		auto close_control = encode_close_metadata(close_metadata_value);
		auto schema_round_trip = schema_control
			? decode_schema_negotiate_metadata(*schema_control)
			: cxxlens::sdk::result<schema_negotiate_metadata>{
				  cxxlens::sdk::unexpected(schema_control.error())};
		auto open_round_trip = open_control ? decode_open_task_metadata(*open_control)
											: cxxlens::sdk::result<open_task_metadata>{
												  cxxlens::sdk::unexpected(open_control.error())};
		auto credit_round_trip = credit_control
			? decode_credit_metadata(*credit_control)
			: cxxlens::sdk::result<credit_metadata>{
				  cxxlens::sdk::unexpected(credit_control.error())};
		auto close_round_trip = close_control
			? decode_close_metadata(*close_control)
			: cxxlens::sdk::result<close_metadata>{cxxlens::sdk::unexpected(close_control.error())};
		require(
			accepted_round_trip && *accepted_round_trip == accepted_metadata && begin_round_trip &&
				*begin_round_trip == begin_metadata && coverage_round_trip &&
				coverage_round_trip->size() == 1U && coverage_round_trip->front().kind == special &&
				coverage_round_trip->front().id == special &&
				coverage_round_trip->front().state == special &&
				coverage_round_trip->front().reason == special && unresolved_round_trip &&
				*unresolved_round_trip ==
					std::vector<unresolved_item>{unresolved_values.begin(),
												 unresolved_values.end()} &&
				evidence_round_trip &&
				*evidence_round_trip ==
					std::vector<evidence_item>{evidence_values.begin(), evidence_values.end()} &&
				canonical_evidence_a && canonical_evidence_b &&
				*canonical_evidence_a == *canonical_evidence_b && !rejected_duplicate_evidence &&
				complete_round_trip && *complete_round_trip == complete_metadata &&
				failed_round_trip && *failed_round_trip == failed_metadata && schema_round_trip &&
				*schema_round_trip == schema_metadata && open_round_trip &&
				*open_round_trip == open_metadata && credit_round_trip &&
				*credit_round_trip == credit_metadata_value && close_round_trip &&
				*close_round_trip == close_metadata_value,
			"structured control metadata did not preserve delimiter, NUL, or UTF-8 fields");

		auto short_map = *accepted_control;
		short_map.front() = std::byte{0xa3};
		auto trailing = *accepted_control;
		trailing.push_back(std::byte{});
		std::vector<std::byte> duplicate_key{std::byte{0xa2}};
		for (std::size_t index = 0U; index < 2U; ++index)
		{
			duplicate_key.push_back(std::byte{0x66});
			for (const auto value : std::string_view{"schema"})
				duplicate_key.push_back(static_cast<std::byte>(value));
			duplicate_key.push_back(std::byte{0x61});
			duplicate_key.push_back(static_cast<std::byte>('x'));
		}
		std::span<const coverage_unit> no_coverage;
		auto invalid_count = encode_coverage_metadata(no_coverage);
		require(invalid_count.has_value(), "empty structured coverage fixture did not encode");
		const std::string_view count_key{"record_count"};
		auto count_key_position = std::ranges::search(
			*invalid_count, std::as_bytes(std::span{count_key.data(), count_key.size()}));
		if (count_key_position.end() != invalid_count->end())
			*count_key_position.end() = std::byte{1U};
		require(!decode_task_accepted_metadata(short_map) &&
					!decode_task_accepted_metadata(trailing) &&
					!decode_task_accepted_metadata(duplicate_key) && invalid_count &&
					!decode_coverage_metadata(*invalid_count),
				"structured control accepted field count, duplicate key, trailing bytes, or bad "
				"record count");

		structured_metadata_provider metadata_a{false};
		structured_metadata_provider metadata_b{true};
		const std::string dependency_special{"dependency|雪"};
		auto metadata_task = make_provider_task(metadata_a,
												task.project,
												{cxxlens::cc::relations::call_site::descriptor()},
												"condition:all",
												"company.test",
												{dependency_special});
		auto metadata_report_a = harness.run(metadata_a, metadata_task);
		auto metadata_report_b = harness.run(metadata_b, metadata_task);
		const auto metadata_frame = [&](const message_type type) -> const frame*
		{
			if (!metadata_report_a)
				return nullptr;
			const auto found = std::ranges::find(metadata_report_a->frames, type, &frame::type);
			return found == metadata_report_a->frames.end() ? nullptr : &*found;
		};
		const auto* begin_frame = metadata_frame(message_type::batch_begin);
		const auto* coverage_frame = metadata_frame(message_type::coverage_chunk);
		const auto* unresolved_frame = metadata_frame(message_type::unresolved_chunk);
		const auto* evidence_frame = metadata_frame(message_type::progress);
		auto decoded_begin = begin_frame
			? decode_batch_begin_metadata(begin_frame->control)
			: cxxlens::sdk::result<batch_begin_metadata>{
				  cxxlens::sdk::unexpected(cxxlens::sdk::error{"sdk.test-setup", "begin", {}})};
		auto decoded_coverage = coverage_frame
			? decode_coverage_metadata(coverage_frame->control)
			: cxxlens::sdk::result<std::vector<coverage_unit>>{
				  cxxlens::sdk::unexpected(cxxlens::sdk::error{"sdk.test-setup", "coverage", {}})};
		auto decoded_unresolved = unresolved_frame
			? decode_unresolved_metadata(unresolved_frame->control)
			: cxxlens::sdk::result<std::vector<unresolved_item>>{cxxlens::sdk::unexpected(
				  cxxlens::sdk::error{"sdk.test-setup", "unresolved", {}})};
		auto decoded_evidence = evidence_frame
			? decode_evidence_metadata(evidence_frame->control)
			: cxxlens::sdk::result<std::vector<evidence_item>>{
				  cxxlens::sdk::unexpected(cxxlens::sdk::error{"sdk.test-setup", "evidence", {}})};
		process_execution_report process_identity_a;
		process_execution_report process_identity_b;
		if (metadata_report_a && metadata_report_b)
		{
			process_identity_a.frames = metadata_report_a->frames;
			process_identity_b.frames = metadata_report_b->frames;
		}
		require(metadata_report_a && metadata_report_a->accepted && metadata_report_b &&
					metadata_report_b->accepted &&
					metadata_report_a->semantic_transcript !=
						metadata_report_b->semantic_transcript &&
					process_identity_a.canonical_form() != process_identity_b.canonical_form() &&
					decoded_begin && decoded_begin->dependency_group_id == dependency_special &&
					decoded_begin->atomic_output_group_id == special &&
					decoded_begin->batch_id == special && decoded_coverage &&
					decoded_coverage->front().reason == special && decoded_unresolved &&
					decoded_unresolved->front().subject == special &&
					decoded_unresolved->front().detail == special && decoded_evidence &&
					decoded_evidence->size() == 2U &&
					decoded_evidence->front().summary != decoded_evidence->back().summary,
				"SDK-generated structured transcript lost delimiters, evidence summary, or digest "
				"identity");
		require(forged && !forged->accepted && forged->reason_code == "provider.task-invalid" &&
					forged->frames.empty() && mutated && !mutated->accepted &&
					mutated->frames.empty() && mismatched_shape && !mismatched_shape->accepted &&
					mismatched_shape->frames.empty() && !duplicate_outputs &&
					!conflicting_outputs && !empty_condition && !invalid_interpretation &&
					invalid_id_report && !invalid_id_report->accepted &&
					invalid_id_report->frames.empty() && invalid_version_report &&
					!invalid_version_report->accepted && invalid_version_report->frames.empty(),
				"invalid task/output/session reached task_accepted");
		require(corrupt && !corrupt->accepted &&
					corrupt->reason_code == "provider.checksum-mismatch" && truncated &&
					!truncated->accepted && truncated->reason_code == "provider.truncated-stream" &&
					cancelled && !cancelled->accepted &&
					cancelled->reason_code == "provider.cancelled" && wrong_direction &&
					!wrong_direction->accepted &&
					wrong_direction->reason_code == "provider.protocol-state-invalid" &&
					missing_terminal && !missing_terminal->accepted && wrong_terminal &&
					!wrong_terminal->accepted && credit_exceeded && !credit_exceeded->accepted &&
					credit_exceeded->reason_code == "provider.credit-exceeded" &&
					invalid_reference && !invalid_reference->accepted &&
					invalid_reference->reason_code == wrong_direction->reason_code,
				"provider harness fault matrix diverged");

		unsealed_provider unsealed_implementation;
		batch_lifecycle_provider begin_only_implementation{"company.test.begin-only-provider",
														   batch_lifecycle_scenario::begin_only};
		batch_lifecycle_provider partial_implementation{
			"company.test.partial-provider", batch_lifecycle_scenario::partially_unsealed};
		batch_lifecycle_provider duplicate_implementation{
			"company.test.duplicate-provider", batch_lifecycle_scenario::duplicate_batch};
		batch_lifecycle_provider interleaved_implementation{
			"company.test.interleaved-provider", batch_lifecycle_scenario::interleaved_sink};
		batch_lifecycle_provider complete_implementation{"company.test.complete-provider",
														 batch_lifecycle_scenario::complete};
		unrequested_descriptor_provider unrequested_implementation;
		incomplete_coverage_provider incomplete_implementation;
		auto unsealed_task = make_provider_task(unsealed_implementation,
												task.project,
												{cxxlens::cc::relations::call_site::descriptor()});
		auto unrequested_task =
			make_provider_task(unrequested_implementation,
							   task.project,
							   {cxxlens::cc::relations::call_site::descriptor()});
		auto incomplete_task =
			make_provider_task(incomplete_implementation,
							   task.project,
							   {cxxlens::cc::relations::call_site::descriptor()});
		auto unsealed = harness.run(unsealed_implementation, unsealed_task);
		const auto run_lifecycle = [&](batch_lifecycle_provider& provider)
		{
			auto provider_task = make_provider_task(
				provider, task.project, {cxxlens::cc::relations::call_site::descriptor()});
			return harness.run(provider, provider_task);
		};
		auto begin_only = run_lifecycle(begin_only_implementation);
		auto partial = run_lifecycle(partial_implementation);
		auto duplicate = run_lifecycle(duplicate_implementation);
		auto interleaved = run_lifecycle(interleaved_implementation);
		auto complete_task = make_provider_task(complete_implementation,
												task.project,
												{cxxlens::cc::relations::call_site::descriptor()});
		auto complete = harness.run(complete_implementation, complete_task);
		auto unrequested = harness.run(unrequested_implementation, unrequested_task);
		auto incomplete_coverage = harness.run(incomplete_implementation, incomplete_task);
		const auto batch_state_rejected = [](const auto& report)
		{
			return report && !report->accepted &&
				report->reason_code == "provider.batch-state-invalid" &&
				std::ranges::none_of(report->frames,
									 [](const auto& frame)
									 {
										 return frame.type ==
											 cxxlens::sdk::provider::message_type::task_complete;
									 });
		};
		const auto report_reason = [](const auto& report)
		{
			return report ? report->reason_code : report.error().code;
		};
		require(batch_state_rejected(unsealed) && batch_state_rejected(begin_only) &&
					batch_state_rejected(partial) && batch_state_rejected(duplicate) &&
					batch_state_rejected(interleaved) && complete && complete->accepted &&
					unrequested && !unrequested->accepted &&
					unrequested->reason_code == "provider.relation-incompatible" &&
					incomplete_coverage && !incomplete_coverage->accepted &&
					incomplete_coverage->reason_code == "provider.coverage-incomplete",
				"provider harness lifecycle, relation, or coverage state diverged: " +
					report_reason(unsealed) + "," + report_reason(begin_only) + "," +
					report_reason(partial) + "," + report_reason(duplicate) + "," +
					report_reason(interleaved) + "," + report_reason(complete) + "," +
					report_reason(unrequested) + "," + report_reason(incomplete_coverage));

		class fail_once_sink final : public cxxlens::sdk::provider::frame_sink
		{
		  public:
			cxxlens::sdk::result<void> write(const std::span<const std::byte> bytes) override
			{
				const auto attempt = attempts++;
				if (attempt == 3U)
					return cxxlens::sdk::unexpected(
						cxxlens::sdk::error{"provider.backpressure", "injected-end", {}});
				transcript.insert(transcript.end(), bytes.begin(), bytes.end());
				return {};
			}
			std::size_t attempts{};
			std::vector<std::byte> transcript;
		};
		fail_once_sink failing_sink;
		cxxlens::sdk::provider::protocol_writer failing_writer{failing_sink};
		failing_writer.grant_credit({std::numeric_limits<std::uint64_t>::max(), 4096U});
		auto end_failure = cxxlens::sdk::provider::run_worker(
			complete_implementation, complete_task, failing_writer);
		auto end_failure_frames =
			cxxlens::sdk::provider::decode_frame_stream(failing_sink.transcript);
		auto end_failure_verdict = end_failure_frames
			? cxxlens::sdk::testing::validate_logical_transcript(
				  complete_task,
				  complete_implementation.id(),
				  complete_implementation.version(),
				  *end_failure_frames,
				  {std::numeric_limits<std::uint64_t>::max(), 4096U})
			: cxxlens::sdk::result<cxxlens::sdk::testing::conformance_report>{
				  cxxlens::sdk::unexpected(
					  cxxlens::sdk::error{"sdk.test-setup", "end-failure", {}})};
		require(
			!end_failure && end_failure.error().code == "provider.backpressure" &&
				end_failure_frames && !end_failure_frames->empty() && end_failure_verdict &&
				!end_failure_verdict->accepted &&
				end_failure_verdict->reason_code == "provider.backpressure" &&
				end_failure_frames->back().type ==
					cxxlens::sdk::provider::message_type::task_failed &&
				std::ranges::none_of(*end_failure_frames,
									 [](const auto& frame)
									 {
										 return frame.type ==
											 cxxlens::sdk::provider::message_type::task_complete;
									 }),
			"end send failure emitted success or omitted task failure");
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
		auto malformed_text_observation = observe(make_direct_target_row("entity:a"));
		malformed_text_observation.interpretation = "domain\nforged";
		auto malformed_text =
			cxxlens::sdk::make_assertion(*engine, std::move(malformed_text_observation));
		require(!malformed_text && malformed_text.error().code == "sdk.text-invalid" &&
					malformed_text.error().field == "interpretation",
				"public claim builder admitted schema-invalid text");
		auto max_length_claim = *first;
		max_length_claim.provenance_root.assign(512U, 'p');
		require(cxxlens::sdk::validate_claim(*engine, max_length_claim).has_value(),
				"claim strong ID rejected the schema maximum length");
		max_length_claim.provenance_root.push_back('p');
		auto over_length = cxxlens::sdk::validate_claim(*engine, max_length_claim);
		require(!over_length && over_length.error().code == "sdk.text-invalid" &&
					over_length.error().detail == "max-length",
				"claim strong ID admitted maxLength + 1");
		auto tampered = *first;
		tampered.content.back() = tampered.content.back() == '0' ? '1' : '0';
		auto tampered_result = cxxlens::sdk::validate_claim(*engine, tampered);
		require(!tampered_result && tampered_result.error().code == "sdk.claim-identity-mismatch",
				"tampered claim identity was accepted");
		auto invalid_stage = *first;
		invalid_stage.stage = static_cast<cxxlens::sdk::claim_stage>(255U);
		auto invalid_stage_result = cxxlens::sdk::validate_claim(*engine, invalid_stage);
		require(!invalid_stage_result &&
					invalid_stage_result.error().code == "sdk.claim-basis-invalid" &&
					invalid_stage_result.error().field == "stage",
				"invalid claim stage passed basis validation");
		const auto rejects_invalid_stage_input =
			[&](const cxxlens::sdk::claim& invalid, const std::string_view subject)
		{
			auto independently_invalid = cxxlens::sdk::validate_claim(*engine, invalid);
			require(!independently_invalid,
					"invalid claim fixture unexpectedly validated: " + std::string{subject});
			auto canonical_rejected = cxxlens::sdk::make_canonical_claim(
				*engine,
				invalid,
				{"company.test.canonicalizer", "company.test.canonicalizer.v1"},
				make_direct_target_row("entity:a"),
				"sha256:1111111111111111111111111111111111111111111111111111111111111111");
			require(!canonical_rejected &&
						canonical_rejected.error().code == independently_invalid.error().code,
					"canonical constructor bypassed input validation: " + std::string{subject});
			const std::array invalid_inputs{invalid};
			auto derived_rejected = cxxlens::sdk::make_derived_claim(
				*engine,
				invalid_inputs,
				observe(make_direct_target_row("entity:a")),
				"snapshot:1",
				{"partition-content:sha256:"
				 "3333333333333333333333333333333333333333333333333333333333333333"},
				"sha256:2222222222222222222222222222222222222222222222222222222222222222");
			require(!derived_rejected &&
						derived_rejected.error().code == independently_invalid.error().code,
					"canonical and derived input validation diverged: " + std::string{subject});
		};
		for (const auto& invalid :
			 [&]
			 {
				 std::vector<cxxlens::sdk::claim> values;
				 auto changed = *first;
				 changed.row = make_direct_target_row("entity:b");
				 values.push_back(changed);
				 changed = *first;
				 changed.semantic_key += "-tampered";
				 values.push_back(changed);
				 changed = *first;
				 changed.assertion += "-tampered";
				 values.push_back(changed);
				 changed = *first;
				 changed.content += "-tampered";
				 values.push_back(changed);
				 changed = *first;
				 changed.presence.fragments = {"debug"};
				 values.push_back(changed);
				 changed = *first;
				 changed.interpretation += ".tampered";
				 values.push_back(changed);
				 changed = *first;
				 changed.producer.semantic_contract += ".tampered";
				 values.push_back(changed);
				 changed = *first;
				 changed.guarantee.approximation = "invalid";
				 values.push_back(changed);
				 changed = *first;
				 changed.presence.universe = std::string{"bad\xff", 4U};
				 values.push_back(changed);
				 changed = *first;
				 changed.presence.fragments = {"all\nforged"};
				 values.push_back(changed);
				 changed = *first;
				 changed.interpretation = "domain\nforged";
				 values.push_back(changed);
				 changed = *first;
				 changed.producer.id = std::string{"\x01provider", 9U};
				 values.push_back(changed);
				 changed = *first;
				 changed.producer.semantic_contract = std::string{"bad\xc3\x28", 5U};
				 values.push_back(changed);
				 changed = *first;
				 changed.provenance_root = "evidence\x7froot";
				 values.push_back(changed);
				 changed = *first;
				 changed.guarantee.scope = "project\nforged";
				 values.push_back(changed);
				 changed = *first;
				 changed.guarantee.assumptions = std::string{"bad\0assumption", 14U};
				 values.push_back(changed);
				 changed = *first;
				 changed.guarantee.verification_modalities = {"schema|validated"};
				 values.push_back(changed);
				 changed = *first;
				 std::get<cxxlens::sdk::direct_claim_basis>(changed.input_basis).basis_digest =
					 "sha256:invalid";
				 values.push_back(changed);
				 changed = *first;
				 std::get<cxxlens::sdk::direct_claim_basis>(changed.input_basis).basis_digest =
					 std::string{"bad\xc3\x28", 5U};
				 values.push_back(changed);
				 return values;
			 }())
			rejects_invalid_stage_input(invalid, "mutated assertion");
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
		auto recanonicalized = cxxlens::sdk::make_canonical_claim(
			*engine,
			*canonical,
			{"company.test.canonicalizer", "company.test.canonicalizer.v1"},
			make_direct_target_row("entity:a"),
			"sha256:1111111111111111111111111111111111111111111111111111111111111111");
		require(!recanonicalized && recanonicalized.error().code == "sdk.claim-stage-invalid",
				"canonical constructor accepted a non-assertion input stage");
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
		auto invalid_derived_text = *derived;
		std::get<cxxlens::sdk::derived_claim_basis>(invalid_derived_text.input_basis)
			.input_snapshot = "snapshot\nforged";
		auto invalid_derived_result = cxxlens::sdk::validate_claim(*engine, invalid_derived_text);
		require(!invalid_derived_result &&
					invalid_derived_result.error().code == "sdk.text-invalid" &&
					invalid_derived_result.error().field == "input_snapshot",
				"derived basis admitted schema-invalid snapshot ID");

		auto call = cxxlens::sdk::make_assertion(
			*engine, observe(make_call_row(), {"all", "asan", "debug", "release"}));
		require(call.has_value(), "call-site assertion rejected");
		auto rebound_subject = *first;
		rebound_subject.semantic_key = call->semantic_key;
		auto rebound_subject_result = cxxlens::sdk::validate_claim(*engine, rebound_subject);
		require(!rebound_subject_result &&
					rebound_subject_result.error().code == "sdk.claim-identity-mismatch",
				"evidence occurrence subject repoint was accepted");
		rebound_subject = *first;
		rebound_subject.content = call->content;
		require(!cxxlens::sdk::validate_claim(*engine, rebound_subject),
				"evidence occurrence content repoint was accepted");
		const std::array existing_call{*call};
		{
			auto provenance_occurrence = *first;
			provenance_occurrence.provenance_root = "evidence:alternate";
			auto producer_occurrence = *first;
			producer_occurrence.producer.id = "company.test.alternate-provider";
			auto guarantee_occurrence = *first;
			guarantee_occurrence.guarantee.approximation = "under_approximation";
			const std::array occurrences{
				*first, provenance_occurrence, producer_occurrence, guarantee_occurrence, *first};
			std::array<std::size_t, occurrences.size()> permutation{0U, 1U, 2U, 3U, 4U};
			std::string reference;
			std::size_t permutation_count{};
			do
			{
				cxxlens::sdk::claim_batch occurrence_batch;
				for (const auto index : permutation)
					require(occurrence_batch.add(occurrences[index]).has_value(),
							"evidence occurrence candidate rejected");
				auto result = std::move(occurrence_batch).commit(*engine, existing_call);
				require(result && result->claims.size() == 4U,
						"claim occurrence law lost metadata or retained an exact duplicate");
				std::string occurrence_projection = result->content_digest;
				for (const auto& value : result->claims)
					occurrence_projection += "\n" + value.producer.id + '|' +
						value.provenance_root + '|' + value.guarantee.approximation;
				if (reference.empty())
					reference = occurrence_projection;
				require(occurrence_projection == reference,
						"claim occurrence result depended on input permutation");
				++permutation_count;
			} while (std::ranges::next_permutation(permutation).found);
			require(permutation_count == 120U,
					"claim occurrence permutation matrix was incomplete");
		}
		cxxlens::sdk::claim_batch hard_missing;
		require(hard_missing.add(*first).has_value(), "hard-reference candidate rejected");
		auto rejected = std::move(hard_missing).commit(*engine);
		require(!rejected && rejected.error().code == "sdk.hard-reference-missing",
				"hard missing reference did not reject the batch");

		cxxlens::sdk::claim_batch soft_missing;
		require(soft_missing.add(*first).has_value(), "soft-reference candidate rejected");
		require(soft_missing.add(*first).has_value(),
				"exact duplicate soft-reference candidate rejected");
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

		const std::array existing_functional{*overlap_a, *call, *other_call};
		cxxlens::sdk::claim_batch incremental_conflict;
		require(incremental_conflict.add(*overlap_b).has_value(),
				"incremental conflict claim rejected");
		auto incremental = std::move(incremental_conflict).commit(*engine, existing_functional);
		require(incremental && incremental->conflicts.size() == 1U &&
					incremental->conflicts.front().overlap_fragments ==
						std::vector<std::string>{"release"},
				"new-vs-existing functional conflict was not classified");

		cxxlens::sdk::claim_batch incremental_same_payload;
		require(incremental_same_payload.add(*same_payload).has_value(),
				"incremental same-payload claim rejected");
		auto no_payload_conflict =
			std::move(incremental_same_payload).commit(*engine, existing_functional);
		require(no_payload_conflict && no_payload_conflict->conflicts.empty(),
				"new-vs-existing matching payload produced a false conflict");

		cxxlens::sdk::claim_batch incremental_disjoint;
		require(incremental_disjoint.add(*disjoint).has_value(),
				"incremental disjoint claim rejected");
		auto no_condition_conflict =
			std::move(incremental_disjoint).commit(*engine, existing_functional);
		require(no_condition_conflict && no_condition_conflict->conflicts.empty(),
				"new-vs-existing disjoint conditions produced a false conflict");

		cxxlens::sdk::claim_batch incremental_differential;
		require(incremental_differential.add(*other_domain).has_value(),
				"incremental differential claim rejected");
		auto differential =
			std::move(incremental_differential).commit(*engine, existing_functional);
		require(differential && differential->conflicts.empty() &&
					differential->differential_disagreements.size() == 1U &&
					differential->differential_disagreements.front().overlap_fragments ==
						std::vector<std::string>{"release"},
				"new-vs-existing cross-domain disagreement was not classified");

		cxxlens::sdk::claim_batch one_shot_batch;
		require(one_shot_batch.add(*overlap_a) && one_shot_batch.add(*overlap_b),
				"one-shot comparison claims rejected");
		auto one_shot = std::move(one_shot_batch).commit(*engine, existing_call);
		require(one_shot && one_shot->conflicts.size() == 1U && incremental,
				"one-shot comparison did not produce one conflict");
		const auto& one_shot_conflict = one_shot->conflicts.front();
		const auto& incremental_record = incremental->conflicts.front();
		require(one_shot_conflict.relation == incremental_record.relation &&
					one_shot_conflict.semantic_key == incremental_record.semantic_key &&
					one_shot_conflict.interpretation == incremental_record.interpretation &&
					one_shot_conflict.overlap_fragments == incremental_record.overlap_fragments &&
					one_shot_conflict.assertions == incremental_record.assertions &&
					one_shot_conflict.contents == incremental_record.contents,
				"one-shot and split-publication conflict classification diverged");

		const std::array duplicate_existing{*overlap_a, *overlap_a, *call};
		cxxlens::sdk::claim_batch duplicate_prior_batch;
		require(duplicate_prior_batch.add(*overlap_b).has_value(),
				"duplicate-prior claim rejected");
		auto duplicate_prior = std::move(duplicate_prior_batch).commit(*engine, duplicate_existing);
		require(duplicate_prior && duplicate_prior->conflicts.size() == 1U,
				"duplicate existing claims duplicated incremental conflict records");

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

int main(const int argc, const char* const argv[])
{
	using test_case = std::pair<std::string_view, void (*)()>;
	const std::array cases{
		test_case{"scalar-value-validation", check_scalar_value_validation},
		test_case{"digest", check_digest},
		test_case{"closed-enum-canonical-value", check_closed_enum_and_canonical_value},
		test_case{"descriptor-binding", check_descriptor_binding},
		test_case{"descriptor-canonical-collections", check_descriptor_canonical_collections},
		test_case{"claim-batch-digest-framing", check_claim_batch_digest_framing},
		test_case{"relation-schema-parity", check_relation_schema_parity},
		test_case{"static-row-view-validation", check_static_row_view_validation},
		test_case{"static-dynamic-query", check_static_dynamic_query},
		test_case{"snapshot-lifetime", check_snapshot_lifetime},
		test_case{"frame-native-escape", check_frame_and_native_escape},
		test_case{"columnar-wire-codec", check_columnar_wire_codec},
		test_case{"relation-sink-batch-state", check_relation_sink_batch_state},
		test_case{"project-catalog-identity", check_project_catalog_identity},
		test_case{"provider-tooling-faults", check_provider_tooling_and_faults},
		test_case{"relation-engine-claim-kernel", check_relation_engine_and_claim_kernel},
	};

	if (argc == 2 && std::string_view{argv[1]} == "--list")
	{
		for (const auto& [name, unused] : cases)
		{
			(void)unused;
			std::cout << name << '\n';
		}
		return 0;
	}
	if (argc != 2)
	{
		std::cerr << "usage: cxxlens-unit-sdk <case>|--list\n";
		return 2;
	}
	const auto selected = std::string_view{argv[1]};
	const auto found = std::ranges::find(cases, selected, &test_case::first);
	if (found == cases.end())
	{
		std::cerr << "unknown SDK test case: " << selected << '\n';
		return 2;
	}
	found->second();
	return 0;
}
