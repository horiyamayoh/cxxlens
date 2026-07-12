#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>

#include <cxxlens/testing.hpp>

#include "../query/query_fixture.hpp"
#include "workspace/provisioning.hpp"

namespace cxxlens::test::search_fixture
{
	class semantic_worker final : public detail::scheduling::worker_port
	{
	  public:
		explicit semantic_worker(const bool incomplete = false) : incomplete_{incomplete} {}
		std::atomic<std::size_t> calls{};

		[[nodiscard]] result<detail::frontend::observation_batch>
		execute(const detail::scheduling::task_request& task, execution_context) override
		{
			const auto ordinal = calls.fetch_add(1U);
			if (incomplete_ && ordinal != 0U)
			{
				error failure;
				failure.code.value = "parse.frontend-failed";
				failure.message = "injected search coverage failure";
				failure.scope = failure_scope::compile_unit;
				return failure;
			}
			detail::frontend::observation_batch batch;
			batch.adapter_id = "clang22.frontend";
			batch.adapter_version = "search-fixture";
			batch.unit = task.parse.unit.id();
			batch.variant = task.parse.unit.variant_id();
			batch.coverage.requested = 1U;
			batch.coverage.parsed = 1U;
			const auto fixture_snapshot = query_fixture::snapshot();
			for (const auto& fact : fixture_snapshot->facts)
			{
				detail::facts::observation_record observation;
				observation.adapter_id = batch.adapter_id;
				observation.adapter_version = batch.adapter_version;
				observation.llvm_major = 22U;
				observation.compile_unit = batch.unit;
				observation.variant = batch.variant;
				observation.kind = fact.kind;
				observation.source = fact.source;
				observation.payload_version = fact.payload_version;
				observation.payload = fact.payload;
				observation.payload["semantic_key"] = fact.stable_key;
				observation.name = fact.name;
				observation.type = fact.type;
				batch.observations.push_back(std::move(observation));
			}
			std::ranges::sort(
				batch.observations,
				[](const detail::facts::observation_record& left,
				   const detail::facts::observation_record& right)
				{
					const auto key = [](const detail::facts::observation_record& value)
					{
						return std::to_string(static_cast<std::uint16_t>(value.kind)) + ":" +
							value.payload.at("semantic_key");
					};
					return key(left) < key(right);
				});
			return batch;
		}

	  private:
		bool incomplete_{};
	};

	struct bound_workspace
	{
		workspace value;
		std::shared_ptr<semantic_worker> worker;
	};

	[[nodiscard]] inline result<bound_workspace> open(const bool incomplete = false,
													  const bool multi_variant = true,
													  const bool reverse = false,
													  execution_context context = {})
	{
		auto fixture = testing::workspace_fixture::cpp(
			"struct Base { virtual void step(); };\n"
			"struct Derived : Base { void step() override; };\n"
			"struct Other { void step(); };\n"
			"void run(Base& base, Derived& derived, Other& other) {\n"
			"  base.step(); derived.step(); other.step();\n"
			"}\n");
		if (incomplete)
			fixture = fixture.add_file("support.cpp", "int support() { return 1; }\n");
		if (multi_variant)
			fixture = reverse ? fixture.add_variant({"second", {"-DSEARCH_SECOND=1"}, {}})
									.add_variant({"first", {"-DSEARCH_FIRST=1"}, {}})
							  : fixture.add_variant({"first", {"-DSEARCH_FIRST=1"}, {}})
									.add_variant({"second", {"-DSEARCH_SECOND=1"}, {}});
		auto opened = fixture.open(std::move(context));
		if (!opened)
			return std::move(opened.error());
		auto worker = std::make_shared<semantic_worker>(incomplete);
		detail::workspace_provisioning_access::set_worker(opened.value(), worker);
		return bound_workspace{std::move(opened.value()), std::move(worker)};
	}
} // namespace cxxlens::test::search_fixture
