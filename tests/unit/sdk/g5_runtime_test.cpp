#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <cxxlens/sdk.hpp>

namespace
{
	using namespace cxxlens::sdk;
	namespace incremental = cxxlens::sdk::incremental;

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

	[[nodiscard]] incremental::input_fingerprint fingerprint()
	{
		return {digest('1'),
				digest('2'),
				digest('3'),
				digest('4'),
				digest('5'),
				digest('6'),
				digest('7'),
				digest('8'),
				digest('9'),
				digest('a'),
				digest('6'),
				digest('7'),
				digest('8'),
				digest('9'),
				"normalizer-v1",
				digest('a'),
				digest('b'),
				"exact"};
	}

	[[nodiscard]] incremental::partition_state state(std::string id)
	{
		return {std::move(id), fingerprint(), digest('c'), digest('d'), false};
	}

	void check_incremental_planning()
	{
		auto first = state("partition:a");
		auto second = state("partition:b");
		const std::array warm_candidates{
			incremental::partition_candidate{second, second},
			incremental::partition_candidate{first, first},
		};
		auto warm = incremental::make_materialization_plan(warm_candidates);
		require(warm && warm->validate() && warm->warm_zero &&
					warm->frontend_provider_executions == 0U &&
					warm->entries.front().partition_id == "partition:a",
				"warm-zero did not produce a canonical zero-provider plan");

		auto changed = first;
		changed.input.source_digest = digest('e');
		const std::array affected_candidates{
			incremental::partition_candidate{changed, first},
			incremental::partition_candidate{second, second},
		};
		auto affected = incremental::make_materialization_plan(affected_candidates);
		require(affected && affected->validate() && !affected->warm_zero &&
					affected->frontend_provider_executions == 1U &&
					affected->entries[0].decision == incremental::action::recompute &&
					affected->entries[0].reason == "sdk.incremental-source-changed" &&
					affected->entries[1].decision == incremental::action::reuse,
				"affected partition invalidation recomputed an unrelated partition");

		const std::array expected_reasons{
			"sdk.incremental-source-changed",
			"sdk.incremental-dependency-changed",
			"sdk.incremental-invocation-changed",
			"sdk.incremental-toolchain-changed",
			"sdk.incremental-condition-universe-changed",
			"sdk.incremental-variant-changed",
			"sdk.incremental-provider-set-changed",
			"sdk.incremental-registry-changed",
			"sdk.incremental-interpretation-policy-changed",
			"sdk.incremental-refresh-policy-changed",
			"sdk.incremental-environment-changed",
			"sdk.incremental-provider-binary-changed",
			"sdk.incremental-provider-semantics-changed",
			"sdk.incremental-relation-descriptor-changed",
			"sdk.incremental-normalizer-changed",
			"sdk.incremental-model-changed",
			"sdk.incremental-assumption-changed",
			"sdk.incremental-precision-changed",
		};
		for (std::size_t index = 0U; index < expected_reasons.size(); ++index)
		{
			auto current = first;
			switch (index)
			{
				case 0U:
					current.input.source_digest = digest('f');
					break;
				case 1U:
					current.input.dependency_digest = digest('f');
					break;
				case 2U:
					current.input.invocation_digest = digest('f');
					break;
				case 3U:
					current.input.toolchain_digest = digest('f');
					break;
				case 4U:
					current.input.condition_universe_digest = digest('f');
					break;
				case 5U:
					current.input.variant_digest = digest('f');
					break;
				case 6U:
					current.input.provider_set_digest = digest('f');
					break;
				case 7U:
					current.input.registry_digest = digest('f');
					break;
				case 8U:
					current.input.interpretation_policy_digest = digest('f');
					break;
				case 9U:
					current.input.refresh_policy_digest = digest('f');
					break;
				case 10U:
					current.input.environment_digest = digest('f');
					break;
				case 11U:
					current.input.provider_binary_digest = digest('f');
					break;
				case 12U:
					current.input.provider_semantics_digest = digest('f');
					break;
				case 13U:
					current.input.relation_descriptor_digest = digest('f');
					break;
				case 14U:
					current.input.normalizer_version = "normalizer-v2";
					break;
				case 15U:
					current.input.model_digest = digest('f');
					break;
				case 16U:
					current.input.assumption_digest = digest('f');
					break;
				case 17U:
					current.input.precision_profile = "sound-under";
					break;
				default:
					std::abort();
			}
			const std::array candidate{incremental::partition_candidate{current, first}};
			auto plan = incremental::make_materialization_plan(candidate);
			require(plan && plan->entries.front().reason == expected_reasons[index],
					"one exact invalidation input was silently ignored");
		}

		auto corrupt = first;
		corrupt.corruption_detected = true;
		const std::array corrupt_candidate{incremental::partition_candidate{corrupt, first}};
		auto corrupt_plan = incremental::make_materialization_plan(corrupt_candidate);
		require(corrupt_plan &&
					corrupt_plan->entries.front().reason == "sdk.incremental-corruption-detected",
				"corrupt prior materialization was reused");

		auto malformed = first;
		malformed.input.registry_digest = "sha256:ABC";
		const std::array malformed_candidate{incremental::partition_candidate{malformed, first}};
		auto rejected = incremental::make_materialization_plan(malformed_candidate);
		require(!rejected && rejected.error().code == "sdk.incremental-digest-invalid",
				"malformed exact-input digest was accepted");
		auto invalid_action = *warm;
		invalid_action.entries.front().decision = static_cast<incremental::action>(255U);
		require(!invalid_action.validate(), "unknown materialization action was accepted");
		auto invalid_reason = *warm;
		invalid_reason.entries.front().reason = "sdk.incremental-unregistered-reason";
		auto invalid_reason_result = invalid_reason.validate();
		require(!invalid_reason_result &&
					invalid_reason_result.error().code == "sdk.incremental-plan-entry-invalid",
				"unregistered materialization reason was accepted");
	}

	[[nodiscard]] incremental::closure_request closure_fixture(const std::uint64_t iterations = 8U,
															   const std::uint64_t edges = 8U)
	{
		return {{"entity:a"},
				"inheritance-subtype-set",
				{{"entity:a", "entity:b", {"release"}, "domain:cpp", {"evidence:ab"}},
				 {"entity:b", "entity:c", {"release"}, "domain:cpp", {"evidence:bc"}},
				 {"entity:c", "entity:d", {"release"}, "domain:cpp", {"evidence:cd"}},
				 {"entity:b", "entity:x", {"debug"}, "domain:cpp", {"evidence:bx"}},
				 {"entity:b", "entity:y", {"release"}, "domain:other", {"evidence:by"}}},
				iterations,
				edges};
	}

	void check_bounded_closure()
	{
		auto request = closure_fixture();
		auto invalid_kind = request;
		invalid_kind.closure_kind = "relation-key-enumeration";
		auto rejected_kind = incremental::bounded_transitive_closure(invalid_kind);
		require(!rejected_kind && rejected_kind.error().code == "sdk.closure-kind-invalid",
				"bounded derived closure accepted a persisted partition kind");
		auto full = incremental::bounded_transitive_closure(request);
		require(full && full->validate() && full->closure_certified && full->unresolved.empty() &&
					full->rows.size() == 3U,
				"bounded recursion did not reach the deterministic least fixpoint");
		auto reversed = request;
		std::ranges::reverse(reversed.edges);
		auto permuted = incremental::bounded_transitive_closure(reversed);
		require(permuted && *permuted == *full,
				"bounded recursion depended on input edge iteration order");

		auto iteration_limited = closure_fixture(1U, 8U);
		auto partial = incremental::bounded_transitive_closure(iteration_limited);
		require(partial && partial->validate() && !partial->closure_certified &&
					!partial->rows.empty() && partial->unresolved.size() == 1U &&
					partial->unresolved.front().code == "sdk.closure-iteration-budget",
				"iteration exhaustion discarded positives or certified partial closure");
		auto omitted_partiality = *partial;
		omitted_partiality.unresolved.clear();
		require(!omitted_partiality.validate(),
				"uncertified closure omitted its structured unresolved reason");

		auto edge_limited = closure_fixture(8U, 1U);
		auto edge_partial = incremental::bounded_transitive_closure(edge_limited);
		require(edge_partial && edge_partial->validate() && !edge_partial->closure_certified &&
					std::ranges::any_of(edge_partial->unresolved,
										[](const error& item)
										{
											return item.code == "sdk.closure-edge-budget";
										}),
				"edge exhaustion was omitted from closure partiality");
		auto duplicate_rows = *full;
		duplicate_rows.rows.push_back(duplicate_rows.rows.front());
		std::ranges::sort(
			duplicate_rows.rows,
			[](const incremental::closure_row& left, const incremental::closure_row& right)
			{
				return std::tie(left.root,
								left.target,
								left.interpretation,
								left.condition_fragments,
								left.evidence) < std::tie(right.root,
														  right.target,
														  right.interpretation,
														  right.condition_fragments,
														  right.evidence);
			});
		require(!duplicate_rows.validate(), "duplicate closure result row was accepted");
		auto swapped_kind = *full;
		swapped_kind.closure_kind = "call-target-set";
		require(!swapped_kind.validate(), "closure result kind was not bound by its digest");

		auto current_a = state("partition:edges-a");
		auto current_b = state("partition:edges-b");
		auto prior_b = current_b;
		prior_b.input.source_digest = digest('e');
		const std::array candidates{
			incremental::partition_candidate{current_a, current_a},
			incremental::partition_candidate{current_b, prior_b},
		};
		auto plan = incremental::make_materialization_plan(candidates);
		require(plan && !plan->warm_zero && plan->frontend_provider_executions == 1U &&
					plan->entries[0].decision == incremental::action::reuse &&
					plan->entries[1].decision == incremental::action::recompute,
				"incremental equivalence fixture did not select exactly one affected partition");
		auto incremental_request = request;
		incremental_request.edges.clear();
		const std::array partition_a_edges{request.edges[0], request.edges[3], request.edges[4]};
		const std::array partition_b_edges{request.edges[1], request.edges[2]};
		for (const auto& entry : plan->entries)
		{
			const std::span<const incremental::closure_edge> selected =
				entry.partition_id == "partition:edges-a"
				? std::span<const incremental::closure_edge>{partition_a_edges}
				: std::span<const incremental::closure_edge>{partition_b_edges};
			incremental_request.edges.insert(
				incremental_request.edges.end(), selected.begin(), selected.end());
		}
		auto incremental_result = incremental::bounded_transitive_closure(incremental_request);
		require(incremental_result && *incremental_result == *full,
				"full recompute and exact-input incremental semantics diverged");
	}

	[[nodiscard]] std::uint64_t median(std::vector<std::uint64_t> values)
	{
		std::ranges::sort(values);
		return values[values.size() / 2U];
	}

	void write_benchmark(const std::string& path)
	{
		constexpr std::size_t partition_count = 2048U;
		constexpr std::size_t edge_count = 512U;
		constexpr std::size_t repetitions = 5U;
		std::vector<incremental::partition_candidate> candidates;
		for (std::size_t index = 0U; index < partition_count; ++index)
		{
			auto value = state("partition:benchmark-" + std::to_string(index));
			candidates.push_back({value, value});
		}
		incremental::closure_request request{
			{"entity:benchmark-0"}, "inheritance-subtype-set", {}, edge_count + 1U, edge_count};
		for (std::size_t index = 0U; index < edge_count; ++index)
			request.edges.push_back({"entity:benchmark-" + std::to_string(index),
									 "entity:benchmark-" + std::to_string(index + 1U),
									 {"release"},
									 "domain:cpp",
									 {"evidence:benchmark-" + std::to_string(index)}});
		std::vector<std::uint64_t> plan_times;
		std::vector<std::uint64_t> closure_times;
		for (std::size_t repetition = 0U; repetition < repetitions; ++repetition)
		{
			const auto plan_begin = std::chrono::steady_clock::now();
			auto plan = incremental::make_materialization_plan(candidates);
			const auto plan_end = std::chrono::steady_clock::now();
			require(plan && plan->warm_zero, "benchmark warm-zero plan failed");
			plan_times.push_back(static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(plan_end - plan_begin)
					.count()));
			const auto closure_begin = std::chrono::steady_clock::now();
			auto closure = incremental::bounded_transitive_closure(request);
			const auto closure_end = std::chrono::steady_clock::now();
			require(closure && closure->closure_certified && closure->rows.size() == edge_count,
					"benchmark closure failed");
			closure_times.push_back(static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(closure_end - closure_begin)
					.count()));
		}
		std::ofstream output{path, std::ios::binary | std::ios::trunc};
		require(output.good(), "benchmark report could not be opened");
		output << "{\"schema\":\"cxxlens.g5-performance.v1\","
			   << "\"fixture\":{\"partitions\":" << partition_count << ",\"edges\":" << edge_count
			   << "},\"method\":{\"clock\":\"steady_clock\",\"repetitions\":" << repetitions
			   << ",\"statistic\":\"median\"},"
			   << "\"budgets\":{\"max_iterations\":" << edge_count + 1U
			   << ",\"max_edges\":" << edge_count << "},"
			   << "\"metrics_us\":{\"warm_zero_plan_median\":" << median(plan_times)
			   << ",\"bounded_closure_median\":" << median(closure_times) << "},"
			   << "\"environment\":{\"compiler\":\"" << __VERSION__ << "\",\"operating_system\":\""
#if defined(__linux__)
			   << "linux"
#elif defined(__APPLE__)
			   << "macos"
#elif defined(_WIN32)
			   << "windows"
#else
			   << "unknown"
#endif
			   << "\",\"architecture\":\""
#if defined(__x86_64__) || defined(_M_X64)
			   << "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
			   << "aarch64"
#else
			   << "unknown"
#endif
			   << "\"}}\n";
		require(output.good(), "benchmark report write failed");
	}
} // namespace

int main(const int argc, char** argv)
{
	check_incremental_planning();
	check_bounded_closure();
	if (argc == 3 && std::string_view{argv[1]} == "--benchmark")
		write_benchmark(argv[2]);
	else
		require(argc == 1, "usage: cxxlens-g5-runtime [--benchmark output.json]");
	return 0;
}
