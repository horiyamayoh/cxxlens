#include <array>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/provider/clang22.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
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

	void check_digest()
	{
		const std::vector<std::byte> empty;
		require(cxxlens::sdk::content_digest(empty) ==
					"sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
				"SHA-256 content digest mismatch");
	}

	void check_static_dynamic_query()
	{
		using relation = cxxlens::cc::relations::call_site;
		auto typed = cxxlens::sdk::query::from<relation>();
		cxxlens::sdk::relation_registry registry;
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
} // namespace

int main()
{
	check_digest();
	check_static_dynamic_query();
	check_snapshot_lifetime();
	check_frame_and_native_escape();
	check_provider_tooling_and_faults();
	return 0;
}
