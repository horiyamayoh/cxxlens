#include <optional>
#include <string>
#include <utility>

#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/sdk/testing.hpp>

namespace
{
	constexpr std::string_view provider_contract{
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
	[[nodiscard]] cxxlens::sdk::detached_cell optional_typed(std::string parameter,
															 std::string value)
	{
		auto output = cxxlens::sdk::detached_cell::typed(std::move(parameter), std::move(value));
		output.type.optional = true;
		return output;
	}

	[[nodiscard]] cxxlens::sdk::detached_cell open_symbol(std::string parameter, std::string value)
	{
		return {{cxxlens::sdk::scalar_kind::open_symbol, std::move(parameter), false},
				cxxlens::sdk::cell_state::present,
				cxxlens::sdk::scalar_value{std::move(value)},
				std::nullopt};
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

		std::string_view semantic_contract_digest() const noexcept override
		{
			return provider_contract;
		}

		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task& task,
									   cxxlens::sdk::provider::context& context) override
		{
			using relation = cxxlens::company::relations::lock_acquire;
			auto output = context.relation(relation::descriptor());
			if (auto begun = output.begin("calls", "locks", "locks-0"); !begun)
				return begun;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::acquire>(cxxlens::sdk::detached_cell::typed(
						 "company_lock_acquire_id", "lock-acquire:main")),
					 builder.set<relation::lock>(
						 cxxlens::sdk::detached_cell::typed("company_lock_id", "lock:main")),
					 builder.set<relation::function>(optional_typed("cc_entity_id", "entity:main")),
					 builder.set<relation::source>(
						 cxxlens::sdk::detached_cell::typed("source_span_id", "span:main")),
					 builder.set<relation::mode>(open_symbol("company.lock-mode/1", "exclusive")),
					 builder.set<relation::ordinal>(
						 cxxlens::sdk::detached_cell::unsigned_integer(0U)),
				 })
				if (!result)
					return result;
			auto row = std::move(builder).finish();
			if (!row)
				return cxxlens::sdk::unexpected(std::move(row.error()));
			if (auto pushed = output.push(std::move(*row)); !pushed)
				return pushed;
			if (auto ended = output.end(); !ended)
				return ended;
			context.coverage().request("task", task.task_id);
			if (auto classified =
					context.coverage().classify({"task", task.task_id, "covered", {}});
				!classified)
				return classified;
			context.evidence().add(
				{"provider.observation", "lock:main", std::string{id()}, "lock acquire"});
			return {};
		}
	};
} // namespace

int main()
{
	lock_provider implementation;
	auto catalog = cxxlens::sdk::project_catalog::make(
		".",
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		{{"unit-1",
		  "sha256:2222222222222222222222222222222222222222222222222222222222222222",
		  "sha256:3333333333333333333333333333333333333333333333333333333333333333",
		  "sha256:1111111111111111111111111111111111111111111111111111111111111111"}});
	if (!catalog)
		return 1;
	const auto descriptor = cxxlens::company::relations::lock_acquire::descriptor();
	auto task =
		cxxlens::sdk::provider::task::make({std::string{implementation.id()},
											implementation.version(),
											std::string{implementation.semantic_contract_digest()},
											{descriptor},
											{},
											{"provider.company.example"},
											"observation",
											"assertion"},
										   std::move(*catalog),
										   {descriptor},
										   "condition:all",
										   "provider.company.example",
										   {"calls"});
	if (!task)
		return 1;
	cxxlens::sdk::testing::provider_harness harness;
	auto report = harness.run(implementation, *task);
	return report && report->accepted && report->frames.size() >= 7U ? 0 : 1;
}
