#include "llvm/clang22/materialization_v4_coordinator.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stop_token>
#include <string_view>
#include <type_traits>

#include <cxxlens/relations/build_project.hpp>

namespace
{
	using namespace cxxlens::detail::clang22::materialization;
	template <class Value>
	concept retains_raw_inherited_authority = requires(Value value) { value.inherited_authority; };
	static_assert(!retains_raw_inherited_authority<materialization_v4_validated_request>);
	static_assert(!std::is_copy_constructible_v<materialization_v4_validated_request>);

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	class prepared_state final : public materialization_v4_store_prepared_state
	{
	  public:
		explicit prepared_state(std::shared_ptr<unsigned> destructions)
			: destructions_{std::move(destructions)}
		{
		}
		~prepared_state() override
		{
			++*destructions_;
		}

	  private:
		std::shared_ptr<unsigned> destructions_;
	};

	struct port_observation
	{
		unsigned calls{};
	};

	class stopped_ports final : public materialization_v4_closure_port,
								public materialization_v4_source_identity_port,
								public materialization_v4_worker_port,
								public materialization_v4_host_claim_port,
								public materialization_v4_store_port,
								public materialization_report2_2_storage_port,
								public materialization_report2_2_projection_port
	{
	  public:
		explicit stopped_ports(port_observation& observation) : observation_{&observation} {}

		cxxlens::sdk::result<std::vector<materialization_v4_admitted_closure>>
		receive_all(const materialization_v4_validated_request&, std::stop_token) override
		{
			return called<std::vector<materialization_v4_admitted_closure>>();
		}

		cxxlens::sdk::result<materialization_v4_worker_result>
		execute(const materialization_v4_task_launch&) override
		{
			return called<materialization_v4_worker_result>();
		}

		cxxlens::sdk::result<void>
		validate_main(const provider_task_v4_base_task&,
					  const provider_task_v4&,
					  const cxxlens::detail::clang22::source_closure_snapshot&,
					  std::stop_token) override
		{
			return called<void>();
		}

		cxxlens::sdk::result<materialization_v4_host_claim_result>
		translate(const materialization_v4_task_launch&,
				  const materialization_v4_worker_result_binding&,
				  const cxxlens::sdk::provider::detail::sealed_provider_transcript&,
				  const cxxlens::sdk::relation_engine&) override
		{
			return called<materialization_v4_host_claim_result>();
		}

		cxxlens::sdk::result<materialization_v4_store_preparation>
		prepare(const materialization_v4_validated_request&,
				const provider_task_v4_request_authority&,
				std::string_view,
				const cxxlens::sdk::relation_engine&,
				const materialization_v4_incremental_receipt&,
				std::span<const materialization_v4_claim_sealed* const>,
				std::span<const materialization_v4_host_claim_result* const>,
				const materialization_v4_coordinator_limits&,
				std::stop_token) override
		{
			return called<materialization_v4_store_preparation>();
		}

		materialization_v4_store_publication_result
		publish_once(materialization_v4_store_preparation, std::stop_token) noexcept override
		{
			++observation_->calls;
			return {};
		}

		cxxlens::sdk::result<std::unique_ptr<materialization_report2_2_reserved_spool>>
		create(materialization_report2_2_limits, std::stop_token) override
		{
			return called<std::unique_ptr<materialization_report2_2_reserved_spool>>();
		}

		cxxlens::sdk::result<void>
		write_prepublication(const materialization_report2_2_prepublication_projection&,
							 materialization_report2_2_chunk_sink&,
							 std::stop_token) override
		{
			return called<void>();
		}

		cxxlens::sdk::result<void>
		write_terminal(const materialization_report2_2_terminal_projection&,
					   materialization_report2_2_chunk_sink&) override
		{
			return called<void>();
		}

		cxxlens::sdk::result<void>
		validate_sealed(const materialization_report2_2_prepublication_projection&,
						const materialization_report2_2_terminal_projection&,
						materialization_replayable_spool&,
						const materialization_report2_2_receipt&) override
		{
			return called<void>();
		}

	  private:
		template <class Value>
		[[nodiscard]] cxxlens::sdk::result<Value> called()
		{
			++observation_->calls;
			return cxxlens::sdk::unexpected(
				cxxlens::sdk::error{"test.port-called", "port", "unexpected"});
		}

		port_observation* observation_;
	};

	[[nodiscard]] materialization_v4_store_preparation_projection projection()
	{
		return {
			"materialization-request:test",
			"semantic-v2:sha256:task-receipt",
			2U,
			8192U,
			"semantic-v2:sha256:store-source",
			"semantic-v2:sha256:sealed-input-replay",
			"semantic-v2:sha256:projection",
			"semantic-v2:sha256:backend-staged-cursor",
			"semantic-v2:sha256:projection",
			"semantic-v2:sha256:journal",
		};
	}

	void exact_projection_seals_move_only_state()
	{
		auto destructions = std::make_shared<unsigned>();
		auto prepared = make_materialization_v4_store_preparation(
			std::make_unique<prepared_state>(destructions), projection());
		require(prepared.has_value() && !prepared->preparation_digest().empty() &&
					prepared->projection().expected_projection_digest ==
						prepared->projection().actual_projection_digest,
				"exact Store preparation was not sealed");
		auto state = std::move(*prepared).take_state();
		require(state != nullptr && *destructions == 0U,
				"Store prepared state was not transferred exactly once");
		state.reset();
		require(*destructions == 1U, "Store prepared state ownership was lost");
	}

	void preparation_digest_is_deterministic()
	{
		auto destructions = std::make_shared<unsigned>();
		auto first = make_materialization_v4_store_preparation(
			std::make_unique<prepared_state>(destructions), projection());
		auto second = make_materialization_v4_store_preparation(
			std::make_unique<prepared_state>(destructions), projection());
		require(first.has_value() && second.has_value() &&
					first->preparation_digest() == second->preparation_digest(),
				"equal Store projections produced different preparation identities");
	}

	void invalid_projection_discards_unpublished_state()
	{
		auto destructions = std::make_shared<unsigned>();
		auto mismatch = projection();
		mismatch.actual_projection_digest = "semantic-v2:sha256:different";
		auto rejected = make_materialization_v4_store_preparation(
			std::make_unique<prepared_state>(destructions), std::move(mismatch));
		require(!rejected && *destructions == 1U,
				"mismatched expected/actual projection retained unpublished state");

		auto oversized = projection();
		materialization_v4_coordinator_limits limits;
		limits.maximum_retained_result_bytes = 4096U;
		oversized.source_bytes = 4097U;
		rejected = make_materialization_v4_store_preparation(
			std::make_unique<prepared_state>(destructions), std::move(oversized), limits);
		require(!rejected && *destructions == 2U,
				"oversized Store source retained unpublished state");

		auto zero_tasks = projection();
		zero_tasks.task_count = 0U;
		rejected = make_materialization_v4_store_preparation(
			std::make_unique<prepared_state>(destructions), std::move(zero_tasks));
		require(!rejected && *destructions == 3U,
				"zero-task Store source retained unpublished state");
	}

	void cancellation_precedes_every_external_port()
	{
		cxxlens::sdk::relation_registry registry;
		require(registry.add(cxxlens::build::relations::project::descriptor()).has_value(),
				"cancellation fixture descriptor was rejected");
		auto engine = registry.build("engine-generation:test");
		require(engine.has_value(), "cancellation fixture engine was rejected");
		std::stop_source cancellation;
		cancellation.request_stop();
		materialization_v4_coordinator_input input{
			{}, {}, {}, std::move(*engine), {}, cancellation.get_token()};
		port_observation observation;
		stopped_ports ports{observation};
		auto result = run_materialization_v4_coordinator(
			std::move(input), {ports, ports, ports, ports, ports, ports, ports});
		require(!result && result.error().code == "materialization.cancelled" &&
					observation.calls == 0U,
				"pre-cancelled request reached an external port");
	}
} // namespace

int main()
{
	exact_projection_seals_move_only_state();
	preparation_digest_is_deterministic();
	invalid_projection_discards_unpublished_state();
	cancellation_precedes_every_external_port();
	return 0;
}
