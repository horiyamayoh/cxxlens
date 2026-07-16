#include <utility>

#include <cxxlens/sdk/testing.hpp>

namespace
{
	[[nodiscard]] cxxlens::sdk::relation_descriptor descriptor()
	{
		cxxlens::sdk::relation_descriptor value;
		value.id = "company.example.lock_observation.v1";
		value.name = "company.example.lock_observation";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.example.lock-observation/1";
		value.owner_namespace = "company.example";
		value.columns = {
			{"company.example.lock_observation.v1.lock",
			 "lock",
			 {cxxlens::sdk::scalar_kind::typed_id, "lock_id", false},
			 true,
			 cxxlens::sdk::column_role::claim_key},
			{"company.example.lock_observation.v1.mode",
			 "mode",
			 {cxxlens::sdk::scalar_kind::utf8_string, {}, false},
			 true,
			 cxxlens::sdk::column_role::authoritative_payload},
		};
		value.key_columns = {"company.example.lock_observation.v1.lock"};
		value.merge = cxxlens::sdk::merge_mode::functional_assertion;
		value.conflict_columns = {"company.example.lock_observation.v1.mode"};
		value.descriptor_digest =
			cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor.v1", value.canonical_form());
		return value;
	}

	class lock_provider final : public cxxlens::sdk::provider::portable_provider
	{
	  public:
		std::string_view id() const noexcept override
		{
			return "company.example.lock-provider";
		}
		cxxlens::sdk::semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task& task,
									   cxxlens::sdk::provider::context& context) override
		{
			auto output = context.relation(descriptor());
			if (auto begun = output.begin("locks", "locks", "locks-0"); !begun)
				return begun;
			cxxlens::sdk::row_builder builder{descriptor()};
			if (auto set = builder.set({descriptor().id,
										"company.example.lock_observation.v1.lock",
										{cxxlens::sdk::scalar_kind::typed_id, "lock_id", false}},
									   cxxlens::sdk::detached_cell::typed("lock_id", "lock:main"));
				!set)
				return set;
			if (auto set = builder.set({descriptor().id,
										"company.example.lock_observation.v1.mode",
										{cxxlens::sdk::scalar_kind::utf8_string, {}, false}},
									   cxxlens::sdk::detached_cell::utf8("exclusive"));
				!set)
				return set;
			auto row = std::move(builder).finish();
			if (!row)
				return cxxlens::sdk::unexpected(std::move(row.error()));
			if (auto pushed = output.push(std::move(*row)); !pushed)
				return pushed;
			if (auto ended = output.end(); !ended)
				return ended;
			context.coverage().request("project", task.project.catalog_id);
			if (auto classified = context.coverage().classify(
					{"project", task.project.catalog_id, "covered", {}});
				!classified)
				return classified;
			context.evidence().add(
				{"provider.observation", "lock:main", std::string{id()}, "example"});
			return {};
		}
	};
} // namespace

int main()
{
	lock_provider implementation;
	cxxlens::sdk::provider::task task;
	task.task_id = "task-1";
	task.project = {"catalog-1", "sha256:catalog", ".", {"unit-1"}};
	task.outputs = {descriptor()};
	task.condition = "condition:all";
	task.interpretation = "provider.company.example";
	cxxlens::sdk::testing::provider_harness harness;
	auto report = harness.run(implementation, task);
	return report && report->accepted && report->frames.size() >= 7U ? 0 : 1;
}
