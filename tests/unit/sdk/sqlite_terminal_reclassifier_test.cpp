#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "sdk/sqlite_terminal_reclassifier_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	constexpr std::array<std::byte, 32U> digest_a{};
	constexpr auto digest_b = []
	{
		std::array<std::byte, 32U> value{};
		value[0] = std::byte{1U};
		return value;
	}();

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::span<const std::byte> bytes(const std::string_view value) noexcept
	{
		return std::as_bytes(std::span{value.data(), value.size()});
	}

	[[nodiscard]] sqlite_committed_generation_maximum none_maximum() noexcept
	{
		return {sqlite_committed_maximum_tag::none, 0U};
	}

	[[nodiscard]] sqlite_committed_generation_maximum
	some_maximum(const std::uint64_t value) noexcept
	{
		return {sqlite_committed_maximum_tag::some, value};
	}

	[[nodiscard]] sqlite_authority_state_view
	state(const sqlite_authority_format format,
		  const std::uint64_t row_count,
		  const sqlite_committed_generation_maximum maximum,
		  const std::string_view canonical,
		  const std::span<const std::byte> digest = digest_a) noexcept
	{
		return {format, row_count, maximum, bytes(canonical), digest};
	}

	[[nodiscard]] sqlite_terminal_authority_observation
	observed(const sqlite_authority_state_view value) noexcept
	{
		return {sqlite_terminal_observation_kind::valid_authority_state, value};
	}

	[[nodiscard]] sqlite_candidate_gate_result
	accept_all(const sqlite_descendant_candidate_witness&, const void*) noexcept
	{
		return sqlite_candidate_gate_result::accepted;
	}

	[[nodiscard]] sqlite_candidate_gate_result
	reject_all(const sqlite_descendant_candidate_witness&, const void*) noexcept
	{
		return sqlite_candidate_gate_result::rejected;
	}

	[[nodiscard]] sqlite_candidate_gate_result
	invalid_gate_result(const sqlite_descendant_candidate_witness&, const void*) noexcept
	{
		return static_cast<sqlite_candidate_gate_result>(255U);
	}

	[[nodiscard]] sqlite_descendant_validation
	accepted_validation(const sqlite_candidate_gate candidate_gate = accept_all,
						const void* context = nullptr) noexcept
	{
		return {sqlite_projection_gate::accepted,
				sqlite_projection_gate::accepted,
				sqlite_projection_gate::accepted,
				sqlite_projection_gate::accepted,
				sqlite_projection_gate::accepted,
				candidate_gate,
				context};
	}

	[[nodiscard]] sqlite_terminal_reclassifier_input
	input(const sqlite_store_operation operation,
		  const sqlite_authority_state_view source,
		  const sqlite_authority_state_view terminal,
		  const std::uint64_t required_population = 0U) noexcept
	{
		return {operation,
				sqlite_main_identity_class::same,
				source,
				std::nullopt,
				observed(terminal),
				accepted_validation(),
				required_population};
	}

	void check_canonical_bytes_own_equality()
	{
		require(sqlite_authority_state_bytes_equal(bytes("same"), bytes("same")),
				"equal canonical state bytes were rejected");
		require(!sqlite_authority_state_bytes_equal(bytes("left"), bytes("right")),
				"same-length unequal canonical bytes compared equal");
		require(!sqlite_authority_state_bytes_equal(bytes("short"), bytes("longer")),
				"different canonical byte counts compared equal");

		const auto source = state(
			sqlite_authority_format::current_v3, 1U, some_maximum(1U), "byte-identical", digest_a);
		const auto same_bytes_different_digest = state(
			sqlite_authority_format::current_v3, 1U, some_maximum(1U), "byte-identical", digest_b);
		auto exact = reclassify_sqlite_terminal(
			input(sqlite_store_operation::publish, source, same_bytes_different_digest));
		require(exact.terminal_class == sqlite_terminal_class::authorized_pre_state &&
					exact.proof.exact_state,
				"a non-authoritative digest difference over equal bytes changed state equality");

		const auto collision = state(sqlite_authority_format::current_v3,
									 1U,
									 some_maximum(1U),
									 "digest-collision",
									 digest_a);
		auto collision_input = input(sqlite_store_operation::publish, source, collision);
		collision_input.descendant_validation.candidate_gate = reject_all;
		const auto collision_result = reclassify_sqlite_terminal(collision_input);
		require(collision_result.terminal_class == sqlite_terminal_class::valid_non_descendant &&
					!collision_result.proof.exact_state,
				"an equal acceleration digest promoted unequal canonical bytes to equality");
	}

	void check_tagged_maximum_and_census_totality()
	{
		const auto v2_some_zero =
			state(sqlite_authority_format::legacy_v2, 1U, some_maximum(0U), "v2-some-zero");
		const auto valid = reclassify_sqlite_terminal(
			input(sqlite_store_operation::migrate_predecessor, v2_some_zero, v2_some_zero));
		require(valid.terminal_class == sqlite_terminal_class::authorized_pre_state,
				"tagged some(0) with one committed row was not a valid authority state");

		const auto v2_none =
			state(sqlite_authority_format::legacy_v2, 0U, none_maximum(), "v2-none");
		require(!sqlite_authority_state_bytes_equal(v2_some_zero.canonical_bytes,
													v2_none.canonical_bytes),
				"tagged none and some(0) lost byte-distinct state identity");

		const auto row_with_none =
			state(sqlite_authority_format::legacy_v2, 1U, none_maximum(), "invalid-row-none");
		auto malformed = input(sqlite_store_operation::migrate_predecessor, v2_none, row_with_none);
		require(reclassify_sqlite_terminal(malformed).terminal_class ==
					sqlite_terminal_class::invalid_census,
				"a committed row with tagged none did not classify as invalid census");

		const auto empty_with_some =
			state(sqlite_authority_format::legacy_v2, 0U, some_maximum(0U), "invalid-empty-some");
		malformed.terminal = observed(empty_with_some);
		require(reclassify_sqlite_terminal(malformed).terminal_class ==
					sqlite_terminal_class::invalid_census,
				"an empty committed set with tagged some did not classify as invalid census");

		const auto impossible_distinct_maximum = state(
			sqlite_authority_format::legacy_v2, 3U, some_maximum(1U), "invalid-distinct-census");
		malformed.terminal = observed(impossible_distinct_maximum);
		require(reclassify_sqlite_terminal(malformed).terminal_class ==
					sqlite_terminal_class::invalid_census,
				"an impossible distinct-generation census was accepted");
	}

	void check_compressed_schedule_vectors()
	{
		const auto huge = solve_sqlite_compaction_residual(1ULL << 63U, 1U, 1U);
		require(huge.status == sqlite_compaction_schedule_status::reachable &&
					huge.residual_run_count == 1U && huge.residual_runs[0].population == 1U &&
					huge.residual_runs[0].count == (1ULL << 63U),
				"huge generation distance was not represented by one compressed run");

		const auto designated = solve_sqlite_compaction_residual(11U, 2U, 4U, 3U);
		require(designated.status == sqlite_compaction_schedule_status::reachable &&
					designated.residual_run_count == 1U &&
					designated.residual_runs[0].population == 4U &&
					designated.residual_runs[0].count == 2U &&
					designated.designated_final_population == 3U,
				"designated final edge was coalesced or reconstructed incorrectly");

		require(solve_sqlite_compaction_residual(1U, 2U, 4U).status ==
					sqlite_compaction_schedule_status::unreachable,
				"an unreachable residual was accepted");
		require(solve_sqlite_compaction_residual(3U, 2U, 4U, 4U).status ==
					sqlite_compaction_schedule_status::unreachable,
				"an oversized designated final edge was accepted");
		require(solve_sqlite_compaction_residual(0U, 0U, 4U).status ==
					sqlite_compaction_schedule_status::invalid_input,
				"zero compaction denomination did not fail closed");
	}

	struct boundary_observation
	{
		mutable bool saw_migration_one_final_two{};
	};

	[[nodiscard]] sqlite_candidate_gate_result
	observe_boundary(const sqlite_descendant_candidate_witness& witness,
					 const void* raw_context) noexcept
	{
		auto& context = *static_cast<const boundary_observation*>(raw_context);
		if (witness.candidate_case ==
				sqlite_descendant_candidate_case::v2_to_v3_final_v3_compaction &&
			witness.migration_population == 1U && witness.last_reset_population == 2U)
		{
			context.saw_migration_one_final_two = witness.residual_run_count == 1U &&
				witness.residual_runs[0] ==
					sqlite_compaction_run{sqlite_compaction_edge_format::legacy_v2, 1U, 1U} &&
				witness.designated_final_edge ==
					sqlite_compaction_run{sqlite_compaction_edge_format::current_v3, 2U, 1U};
		}
		return sqlite_candidate_gate_result::accepted;
	}

	void check_authorized_descendant_witnesses()
	{
		const auto v3_one =
			state(sqlite_authority_format::current_v3, 1U, some_maximum(1U), "v3-one");
		const auto v3_publish =
			state(sqlite_authority_format::current_v3, 2U, some_maximum(2U), "v3-publish");
		const auto publish =
			reclassify_sqlite_terminal(input(sqlite_store_operation::publish, v3_one, v3_publish));
		require(publish.terminal_class ==
						sqlite_terminal_class::authorized_post_state_with_operation_edge &&
					publish.proof.added_publish_count == 1U &&
					publish.proof.canonical_reporting_witness->candidate_case ==
						sqlite_descendant_candidate_case::same_format_no_reset,
				"same-format strict publish extension was not authorized");

		const auto v3_compacted =
			state(sqlite_authority_format::current_v3, 1U, some_maximum(2U), "v3-compacted");
		const auto compact = reclassify_sqlite_terminal(
			input(sqlite_store_operation::compact_current, v3_one, v3_compacted, 1U));
		require(compact.required_operation_edge_present && compact.proof.v3_compaction_present &&
					compact.proof.canonical_reporting_witness->candidate_case ==
						sqlite_descendant_candidate_case::same_format_final_compaction,
				"same-format final compaction edge was not proved");

		const auto huge_target = state(sqlite_authority_format::current_v3,
									   1U,
									   some_maximum((1ULL << 63U) + 1U),
									   "v3-huge-distance");
		const auto huge = reclassify_sqlite_terminal(
			input(sqlite_store_operation::compact_current, v3_one, huge_target, 1U));
		const auto& huge_witness = *huge.proof.canonical_reporting_witness;
		require(huge.proof.accepted && huge_witness.residual_run_count == 1U &&
					huge_witness.residual_runs[0].population == 1U &&
					huge_witness.residual_runs[0].count == (1ULL << 63U) - 1U,
				"descendant proof replayed or lost the huge compressed generation distance");

		const auto v2_one =
			state(sqlite_authority_format::legacy_v2, 1U, some_maximum(1U), "v2-one");
		const auto migration_target =
			state(sqlite_authority_format::current_v3, 1U, some_maximum(2U), "migration-target");
		const auto migration = reclassify_sqlite_terminal(
			input(sqlite_store_operation::migrate_predecessor, v2_one, migration_target));
		require(migration.required_operation_edge_present && migration.proof.migration_present &&
					!migration.proof.legacy_v2_compaction_present &&
					!migration.proof.v3_compaction_present &&
					migration.proof.canonical_reporting_witness->candidate_case ==
						sqlite_descendant_candidate_case::v2_to_v3_migration_last,
				"migration-last candidate lost its exact edge census");

		const auto v2_empty =
			state(sqlite_authority_format::legacy_v2, 0U, none_maximum(), "v2-empty");
		const auto v3_empty =
			state(sqlite_authority_format::current_v3, 0U, none_maximum(), "v3-empty");
		const auto zero_migration = reclassify_sqlite_terminal(
			input(sqlite_store_operation::migrate_predecessor, v2_empty, v3_empty));
		require(zero_migration.proof.accepted && zero_migration.proof.migration_present &&
					!zero_migration.proof.legacy_v2_compaction_present &&
					!zero_migration.proof.v3_compaction_present &&
					zero_migration.proof.canonical_reporting_witness->migration_population == 0U,
				"zero-population migration allocated a compaction denomination");

		const auto two_representation_target = state(sqlite_authority_format::current_v3,
													 1U,
													 some_maximum(3U),
													 "migration-two-representations");
		const auto two = reclassify_sqlite_terminal(
			input(sqlite_store_operation::migrate_predecessor, v2_one, two_representation_target));
		require(two.proof.legacy_v2_compaction_present && two.proof.migration_present &&
					two.proof.v3_compaction_present &&
					two.proof.canonical_reporting_witness->candidate_case ==
						sqlite_descendant_candidate_case::v2_to_v3_migration_last,
				"edge existence was derived only from one migration representation");

		boundary_observation boundary;
		const auto boundary_target =
			state(sqlite_authority_format::current_v3, 2U, some_maximum(6U), "migration-boundary");
		auto boundary_input =
			input(sqlite_store_operation::migrate_predecessor, v2_one, boundary_target);
		boundary_input.descendant_validation = accepted_validation(observe_boundary, &boundary);
		const auto boundary_result = reclassify_sqlite_terminal(boundary_input);
		require(boundary_result.proof.accepted && boundary.saw_migration_one_final_two &&
					boundary_result.proof.legacy_v2_compaction_present &&
					boundary_result.proof.v3_compaction_present,
				"migration-boundary commutation or canonical run tagging was incomplete");
	}

	void check_operation_edge_classification()
	{
		const auto v2_one =
			state(sqlite_authority_format::legacy_v2, 1U, some_maximum(1U), "edge-v2-one");
		const auto v2_publish =
			state(sqlite_authority_format::legacy_v2, 2U, some_maximum(2U), "edge-v2-publish");
		const auto migration_without_edge = reclassify_sqlite_terminal(
			input(sqlite_store_operation::migrate_predecessor, v2_one, v2_publish));
		require(migration_without_edge.terminal_class ==
						sqlite_terminal_class::authorized_post_state_without_operation_edge &&
					!migration_without_edge.required_operation_edge_present,
				"a predecessor-only descendant inferred a migration result edge");

		const auto v3_one =
			state(sqlite_authority_format::current_v3, 1U, some_maximum(1U), "edge-v3-one");
		const auto v3_publish =
			state(sqlite_authority_format::current_v3, 2U, some_maximum(2U), "edge-v3-publish");
		const auto compact_without_edge = reclassify_sqlite_terminal(
			input(sqlite_store_operation::compact_current, v3_one, v3_publish, 1U));
		require(compact_without_edge.terminal_class ==
						sqlite_terminal_class::authorized_post_state_without_operation_edge &&
					!compact_without_edge.required_operation_edge_present,
				"a publish-only descendant inferred a v3 compaction result edge");

		const auto expected_empty =
			state(sqlite_authority_format::current_v3, 0U, none_maximum(), "fresh-expected-empty");
		auto fresh_exact =
			input(sqlite_store_operation::fresh_initialization, expected_empty, expected_empty);
		fresh_exact.expected_candidate = expected_empty;
		const auto fresh_exact_result = reclassify_sqlite_terminal(fresh_exact);
		require(fresh_exact_result.exact_expected_candidate &&
					fresh_exact_result.required_operation_edge_present &&
					fresh_exact_result.terminal_class ==
						sqlite_terminal_class::authorized_post_state_with_operation_edge,
				"fresh expected empty-v3 projection did not prove initialized authority");

		const auto fresh_published =
			state(sqlite_authority_format::current_v3, 1U, some_maximum(1U), "fresh-descendant");
		auto fresh_descendant =
			input(sqlite_store_operation::fresh_initialization, expected_empty, fresh_published);
		fresh_descendant.expected_candidate = expected_empty;
		require(reclassify_sqlite_terminal(fresh_descendant).terminal_class ==
					sqlite_terminal_class::authorized_post_state_with_operation_edge,
				"fresh authorized descendant did not recover initialization authority");
	}

	struct oracle_node
	{
		sqlite_authority_format format{sqlite_authority_format::legacy_v2};
		std::uint64_t rows{};
		std::uint64_t maximum{};
		std::uint8_t edge_mask{};

		[[nodiscard]] bool operator==(const oracle_node&) const = default;
	};

	struct oracle_result
	{
		bool accepted{};
		std::uint8_t edge_mask{};
	};

	[[nodiscard]] oracle_result
	brute_force_scalar_closure(const sqlite_authority_format source_format,
							   const sqlite_authority_format target_format,
							   const std::uint64_t source_rows,
							   const std::uint64_t source_maximum,
							   const std::uint64_t target_rows,
							   const std::uint64_t target_maximum)
	{
		std::vector<oracle_node> queue{{source_format, source_rows, source_maximum, 0U}};
		auto enqueue = [&](const oracle_node& candidate)
		{
			if (candidate.rows <= target_rows && candidate.maximum <= target_maximum)
			{
				bool present{};
				for (const auto& existing : queue)
					present |= existing == candidate;
				if (!present)
					queue.push_back(candidate);
			}
		};

		oracle_result result;
		for (std::size_t index = 0U; index < queue.size(); ++index)
		{
			const auto current = queue[index];
			if (current.format == target_format && current.rows == target_rows &&
				current.maximum == target_maximum)
			{
				result.accepted = true;
				result.edge_mask = static_cast<std::uint8_t>(result.edge_mask | current.edge_mask);
			}

			if (current.rows < target_rows && current.maximum < target_maximum)
				enqueue(
					{current.format, current.rows + 1U, current.maximum + 1U, current.edge_mask});
			if (current.rows != 0U && current.rows <= target_maximum - current.maximum)
			{
				const auto edge = current.format == sqlite_authority_format::legacy_v2 ? 1U : 4U;
				enqueue({current.format,
						 current.rows,
						 current.maximum + current.rows,
						 static_cast<std::uint8_t>(current.edge_mask | edge)});
			}
			if (current.format == sqlite_authority_format::legacy_v2 &&
				current.rows <= target_maximum - current.maximum)
			{
				enqueue({sqlite_authority_format::current_v3,
						 current.rows,
						 current.maximum + current.rows,
						 static_cast<std::uint8_t>(current.edge_mask | 2U)});
			}
		}
		return result;
	}

	void check_small_domain_against_edge_replay_oracle()
	{
		constexpr std::array formats{sqlite_authority_format::legacy_v2,
									 sqlite_authority_format::current_v3};
		for (const auto source_format : formats)
		{
			for (const auto target_format : formats)
			{
				for (std::uint64_t source_rows = 0U; source_rows <= 3U; ++source_rows)
				{
					const auto minimum_source_maximum = source_rows == 0U ? 0U : source_rows - 1U;
					const auto last_source_maximum = source_rows == 0U ? 0U : 4U;
					for (auto source_maximum = minimum_source_maximum;
						 source_maximum <= last_source_maximum;
						 ++source_maximum)
					{
						for (auto target_rows = source_rows; target_rows <= 3U; ++target_rows)
						{
							const auto minimum_target_maximum =
								target_rows == 0U ? 0U : target_rows - 1U;
							const auto last_target_maximum = target_rows == 0U ? 0U : 8U;
							for (auto target_maximum = minimum_target_maximum;
								 target_maximum <= last_target_maximum;
								 ++target_maximum)
							{
								const bool summaries_equal = source_format == target_format &&
									source_rows == target_rows && source_maximum == target_maximum;
								const auto source_state =
									state(source_format,
										  source_rows,
										  source_rows == 0U ? none_maximum()
															: some_maximum(source_maximum),
										  "oracle-source");
								const auto target_state =
									state(target_format,
										  target_rows,
										  target_rows == 0U ? none_maximum()
															: some_maximum(target_maximum),
										  summaries_equal ? "oracle-source" : "oracle-target");
								const auto operation =
									source_format == sqlite_authority_format::legacy_v2
									? sqlite_store_operation::migrate_predecessor
									: sqlite_store_operation::compact_current;
								const auto actual = reclassify_sqlite_terminal(input(
									operation,
									source_state,
									target_state,
									operation == sqlite_store_operation::compact_current ? 1U
																						 : 0U));
								const auto expected = brute_force_scalar_closure(source_format,
																				 target_format,
																				 source_rows,
																				 source_maximum,
																				 target_rows,
																				 target_maximum);
								require(
									actual.proof.accepted == expected.accepted,
									"closed-form acceptance differs from the small replay oracle");
								if (!expected.accepted)
									continue;
								require(
									actual.proof.legacy_v2_compaction_present ==
											((expected.edge_mask & 1U) != 0U) &&
										actual.proof.migration_present ==
											((expected.edge_mask & 2U) != 0U) &&
										actual.proof.v3_compaction_present ==
											((expected.edge_mask & 4U) != 0U),
									"closed-form existential edge bits differ from replay oracle");
							}
						}
					}
				}
			}
		}
	}

	void check_terminal_observation_table()
	{
		const auto dummy = state(sqlite_authority_format::current_v3, 0U, none_maximum(), "dummy");
		struct row
		{
			sqlite_terminal_observation_kind observation;
			sqlite_store_operation operation;
			sqlite_terminal_class expected;
		};
		constexpr std::array rows{
			row{sqlite_terminal_observation_kind::exact_logical_empty_preauthority,
				sqlite_store_operation::fresh_initialization,
				sqlite_terminal_class::exact_logical_empty},
			row{sqlite_terminal_observation_kind::exact_logical_empty_preauthority,
				sqlite_store_operation::publish,
				sqlite_terminal_class::mixed_or_ambiguous},
			row{sqlite_terminal_observation_kind::invalid_census,
				sqlite_store_operation::migrate_predecessor,
				sqlite_terminal_class::invalid_census},
			row{sqlite_terminal_observation_kind::mixed_or_ambiguous,
				sqlite_store_operation::compact_current,
				sqlite_terminal_class::mixed_or_ambiguous},
			row{sqlite_terminal_observation_kind::unavailable,
				sqlite_store_operation::publish,
				sqlite_terminal_class::reclassifier_unavailable},
		};
		for (const auto& value : rows)
		{
			sqlite_terminal_reclassifier_input terminal_input;
			terminal_input.operation = value.operation;
			terminal_input.main_identity = sqlite_main_identity_class::same;
			terminal_input.source = dummy;
			terminal_input.terminal = {value.observation, std::nullopt};
			require(reclassify_sqlite_terminal(terminal_input).terminal_class == value.expected,
					"terminal observation table precedence drifted");
		}

		auto identity = input(sqlite_store_operation::publish, dummy, dummy);
		identity.main_identity = sqlite_main_identity_class::changed;
		require(reclassify_sqlite_terminal(identity).terminal_class ==
					sqlite_terminal_class::valid_non_descendant,
				"changed main identity was admitted by byte equality");
		identity.main_identity = sqlite_main_identity_class::unavailable;
		require(reclassify_sqlite_terminal(identity).terminal_class ==
					sqlite_terminal_class::reclassifier_unavailable,
				"unavailable main identity did not fail closed");
	}

	void check_negative_inputs_fail_closed()
	{
		const auto v3_one =
			state(sqlite_authority_format::current_v3, 1U, some_maximum(1U), "negative-v3-one");
		const auto v3_two =
			state(sqlite_authority_format::current_v3, 1U, some_maximum(2U), "negative-v3-two");
		auto value = input(sqlite_store_operation::compact_current, v3_one, v3_two, 1U);

		value.operation = static_cast<sqlite_store_operation>(255U);
		require(reclassify_sqlite_terminal(value).failure ==
					sqlite_terminal_reclassifier_failure::invalid_operation,
				"invalid operation enum was not typed and fail-closed");
		value = input(sqlite_store_operation::compact_current, v3_one, v3_two, 1U);
		value.main_identity = static_cast<sqlite_main_identity_class>(255U);
		require(reclassify_sqlite_terminal(value).failure ==
					sqlite_terminal_reclassifier_failure::invalid_identity_class,
				"invalid identity enum was not typed and fail-closed");

		value = input(sqlite_store_operation::compact_current, v3_one, v3_two, 1U);
		value.terminal.kind = static_cast<sqlite_terminal_observation_kind>(255U);
		require(reclassify_sqlite_terminal(value).failure ==
					sqlite_terminal_reclassifier_failure::invalid_observation,
				"invalid observation enum was not typed and fail-closed");

		value = input(sqlite_store_operation::compact_current, v3_one, v3_two, 1U);
		value.descendant_validation.logical_extension = static_cast<sqlite_projection_gate>(255U);
		require(reclassify_sqlite_terminal(value).failure ==
					sqlite_terminal_reclassifier_failure::invalid_projection_gate,
				"invalid projection gate enum was not typed and fail-closed");

		value = input(sqlite_store_operation::compact_current, v3_one, v3_two, 1U);
		value.descendant_validation.candidate_gate = nullptr;
		require(reclassify_sqlite_terminal(value).failure ==
					sqlite_terminal_reclassifier_failure::missing_candidate_gate,
				"a nonexact relation without a candidate validator was admitted");

		value = input(sqlite_store_operation::compact_current, v3_one, v3_two, 1U);
		value.descendant_validation.candidate_gate = invalid_gate_result;
		require(reclassify_sqlite_terminal(value).failure ==
					sqlite_terminal_reclassifier_failure::invalid_candidate_gate_result,
				"an invalid candidate result enum was not typed and fail-closed");

		value = input(sqlite_store_operation::compact_current, v3_one, v3_two, 1U);
		value.descendant_validation.physical_projection = sqlite_projection_gate::rejected;
		require(reclassify_sqlite_terminal(value).terminal_class ==
					sqlite_terminal_class::valid_non_descendant,
				"a rejected byte projection gate did not reject descendant authority");

		const auto invalid_digest = state(sqlite_authority_format::current_v3,
										  1U,
										  some_maximum(1U),
										  "invalid-digest",
										  std::span{digest_a}.first<31U>());
		value = input(sqlite_store_operation::compact_current, invalid_digest, v3_two, 1U);
		require(reclassify_sqlite_terminal(value).failure ==
					sqlite_terminal_reclassifier_failure::invalid_source_projection,
				"a malformed acceleration key was not rejected as an invalid source view");

		const auto decreasing_target =
			state(sqlite_authority_format::current_v3, 1U, some_maximum(0U), "decreasing-target");
		value = input(sqlite_store_operation::compact_current, v3_one, decreasing_target, 1U);
		require(reclassify_sqlite_terminal(value).terminal_class ==
					sqlite_terminal_class::valid_non_descendant,
				"a decreasing generation equation was accepted");

		const auto v3_max = state(sqlite_authority_format::current_v3,
								  1U,
								  some_maximum(std::numeric_limits<std::uint64_t>::max()),
								  "maximum-generation");
		const auto invalid_publish_candidate =
			state(sqlite_authority_format::current_v3,
				  2U,
				  some_maximum(std::numeric_limits<std::uint64_t>::max()),
				  "overflowed-publish-candidate");
		value = input(sqlite_store_operation::publish, v3_max, invalid_publish_candidate);
		value.expected_candidate = invalid_publish_candidate;
		require(reclassify_sqlite_terminal(value).failure ==
					sqlite_terminal_reclassifier_failure::invalid_candidate_projection,
				"a candidate generation-allocation overflow was not rejected");

		value = input(sqlite_store_operation::publish, v3_one, v3_two);
		value.required_v3_compaction_population = 1U;
		require(reclassify_sqlite_terminal(value).failure ==
					sqlite_terminal_reclassifier_failure::invalid_operation,
				"a compaction-only edge parameter was accepted for publish");
	}
} // namespace

int main()
{
	check_canonical_bytes_own_equality();
	check_tagged_maximum_and_census_totality();
	check_compressed_schedule_vectors();
	check_authorized_descendant_witnesses();
	check_operation_edge_classification();
	check_small_domain_against_edge_replay_oracle();
	check_terminal_observation_table();
	check_negative_inputs_fail_closed();
	std::cout << "sqlite terminal reclassifier tests passed\n";
	return 0;
}
