#include "sqlite_terminal_reclassifier_internal.hpp"

#include <algorithm>
#include <limits>

namespace cxxlens::sdk
{
	namespace
	{
		enum class state_validation : std::uint8_t
		{
			valid,
			invalid_enum,
			invalid_census,
			invalid_encoding,
		};

		struct proof_calculation
		{
			sqlite_authorized_descendant_proof proof;
			sqlite_terminal_reclassifier_failure failure{
				sqlite_terminal_reclassifier_failure::none};
		};

		[[nodiscard]] constexpr bool valid(const sqlite_store_operation value) noexcept
		{
			switch (value)
			{
				case sqlite_store_operation::fresh_initialization:
				case sqlite_store_operation::publish:
				case sqlite_store_operation::migrate_predecessor:
				case sqlite_store_operation::compact_current:
					return true;
				case sqlite_store_operation::wal_recovery_handoff:
					return false;
			}
			return false;
		}

		[[nodiscard]] constexpr bool valid(const sqlite_authority_format value) noexcept
		{
			switch (value)
			{
				case sqlite_authority_format::legacy_v2:
				case sqlite_authority_format::current_v3:
					return true;
			}
			return false;
		}

		[[nodiscard]] constexpr bool valid(const sqlite_committed_maximum_tag value) noexcept
		{
			switch (value)
			{
				case sqlite_committed_maximum_tag::none:
				case sqlite_committed_maximum_tag::some:
					return true;
			}
			return false;
		}

		[[nodiscard]] constexpr bool valid(const sqlite_terminal_observation_kind value) noexcept
		{
			switch (value)
			{
				case sqlite_terminal_observation_kind::exact_logical_empty_preauthority:
				case sqlite_terminal_observation_kind::valid_authority_state:
				case sqlite_terminal_observation_kind::invalid_census:
				case sqlite_terminal_observation_kind::mixed_or_ambiguous:
				case sqlite_terminal_observation_kind::unavailable:
					return true;
			}
			return false;
		}

		[[nodiscard]] constexpr bool valid(const sqlite_main_identity_class value) noexcept
		{
			switch (value)
			{
				case sqlite_main_identity_class::same:
				case sqlite_main_identity_class::changed:
				case sqlite_main_identity_class::unavailable:
					return true;
			}
			return false;
		}

		[[nodiscard]] constexpr bool valid(const sqlite_projection_gate value) noexcept
		{
			switch (value)
			{
				case sqlite_projection_gate::rejected:
				case sqlite_projection_gate::accepted:
					return true;
			}
			return false;
		}

		[[nodiscard]] constexpr bool valid(const sqlite_candidate_gate_result value) noexcept
		{
			switch (value)
			{
				case sqlite_candidate_gate_result::rejected:
				case sqlite_candidate_gate_result::accepted:
					return true;
			}
			return false;
		}

		[[nodiscard]] state_validation
		validate_state(const sqlite_authority_state_view& state) noexcept
		{
			if (!valid(state.format) || !valid(state.committed_generation_maximum.tag))
				return state_validation::invalid_enum;
			if (state.canonical_bytes.empty())
				return state_validation::invalid_encoding;
			if (!state.acceleration_digest.empty() && state.acceleration_digest.size() != 32U)
				return state_validation::invalid_encoding;

			const auto maximum = state.committed_generation_maximum;
			if (maximum.tag == sqlite_committed_maximum_tag::none)
			{
				if (state.committed_row_count != 0U || maximum.value != 0U)
					return state_validation::invalid_census;
				return state_validation::valid;
			}
			if (state.committed_row_count == 0U)
				return state_validation::invalid_census;
			// Fully validated committed generations are distinct logical u64 values.
			if (maximum.value < state.committed_row_count - 1U)
				return state_validation::invalid_census;
			return state_validation::valid;
		}

		[[nodiscard]] constexpr std::uint64_t
		equation_origin(const sqlite_authority_state_view& state) noexcept
		{
			return state.committed_generation_maximum.tag == sqlite_committed_maximum_tag::none
				? 0U
				: state.committed_generation_maximum.value;
		}

		[[nodiscard]] constexpr bool same_summary(const sqlite_authority_state_view& left,
												  const sqlite_authority_state_view& right) noexcept
		{
			return left.format == right.format &&
				left.committed_row_count == right.committed_row_count &&
				left.committed_generation_maximum == right.committed_generation_maximum;
		}

		[[nodiscard]] bool exact_state(const sqlite_authority_state_view& left,
									   const sqlite_authority_state_view& right) noexcept
		{
			return same_summary(left, right) &&
				sqlite_authority_state_bytes_equal(left.canonical_bytes, right.canonical_bytes);
		}

		[[nodiscard]] bool checked_add(const std::uint64_t left,
									   const std::uint64_t right,
									   std::uint64_t& result) noexcept
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return false;
			result = left + right;
			return true;
		}

		[[nodiscard]] bool checked_accumulate_product(const std::uint64_t population,
													  const std::uint64_t count,
													  std::uint64_t& sum) noexcept
		{
			if (count != 0U && population > std::numeric_limits<std::uint64_t>::max() / count)
				return false;
			const auto product = population * count;
			return checked_add(sum, product, sum);
		}

		[[nodiscard]] bool
		valid_candidate_projection(const sqlite_store_operation operation,
								   const sqlite_authority_state_view& source,
								   const sqlite_authority_state_view& candidate) noexcept
		{
			if (candidate.format != sqlite_authority_format::current_v3)
				return false;

			std::uint64_t expected_count{};
			std::uint64_t expected_maximum{};
			switch (operation)
			{
				case sqlite_store_operation::fresh_initialization:
					return candidate.committed_row_count == 0U &&
						candidate.committed_generation_maximum.tag ==
						sqlite_committed_maximum_tag::none;
				case sqlite_store_operation::publish:
					if (!checked_add(source.committed_row_count, 1U, expected_count) ||
						!checked_add(equation_origin(source), 1U, expected_maximum))
						return false;
					return candidate.committed_row_count == expected_count &&
						candidate.committed_generation_maximum ==
						sqlite_committed_generation_maximum{sqlite_committed_maximum_tag::some,
															expected_maximum};
				case sqlite_store_operation::migrate_predecessor:
				case sqlite_store_operation::compact_current:
					if (candidate.committed_row_count != source.committed_row_count)
						return false;
					if (source.committed_row_count == 0U)
						return candidate.committed_generation_maximum.tag ==
							sqlite_committed_maximum_tag::none;
					if (!checked_add(
							equation_origin(source), source.committed_row_count, expected_maximum))
						return false;
					return candidate.committed_generation_maximum ==
						sqlite_committed_generation_maximum{sqlite_committed_maximum_tag::some,
															expected_maximum};
				case sqlite_store_operation::wal_recovery_handoff:
					return false;
			}
			return false;
		}

		[[nodiscard]] constexpr bool
		gates_have_valid_enums(const sqlite_descendant_validation& validation) noexcept
		{
			return valid(validation.logical_extension) && valid(validation.diagnostic_projection) &&
				valid(validation.reset_rank_and_topology) &&
				valid(validation.physical_projection) &&
				valid(validation.migration_boundary_commutation);
		}

		[[nodiscard]] constexpr bool
		common_gates_accepted(const sqlite_descendant_validation& validation) noexcept
		{
			return validation.logical_extension == sqlite_projection_gate::accepted &&
				validation.diagnostic_projection == sqlite_projection_gate::accepted &&
				validation.reset_rank_and_topology == sqlite_projection_gate::accepted &&
				validation.physical_projection == sqlite_projection_gate::accepted;
		}

		[[nodiscard]] bool schedule_is_reachable(const std::uint64_t delta,
												 const std::uint64_t lower,
												 const std::uint64_t upper) noexcept
		{
			return solve_sqlite_compaction_residual(delta, lower, upper).status ==
				sqlite_compaction_schedule_status::reachable;
		}

		[[nodiscard]] bool forced_population_reachable(const std::uint64_t delta,
													   const std::uint64_t lower,
													   const std::uint64_t upper,
													   const std::uint64_t forced) noexcept
		{
			if (lower == 0U || upper < lower || forced < lower || forced > upper || forced > delta)
				return false;
			return schedule_is_reachable(delta - forced, lower, upper);
		}

		template <typename Function>
		void
		for_each_inclusive(const std::uint64_t first, const std::uint64_t last, Function&& function)
		{
			if (first > last)
				return;
			auto value = first;
			for (;;)
			{
				function(value);
				if (value == last)
					return;
				++value;
			}
		}

		[[nodiscard]] sqlite_descendant_candidate_witness
		make_witness(const sqlite_descendant_candidate_case candidate_case,
					 const std::optional<std::uint64_t> migration_population,
					 const sqlite_descendant_reset_kind reset_kind,
					 const std::optional<std::uint64_t> last_reset_population,
					 const sqlite_compaction_schedule& schedule,
					 const sqlite_compaction_edge_format residual_format,
					 const std::optional<sqlite_compaction_run> final_edge = std::nullopt) noexcept
		{
			sqlite_descendant_candidate_witness witness;
			witness.candidate_case = candidate_case;
			witness.migration_population = migration_population;
			witness.last_reset_kind = reset_kind;
			witness.last_reset_population = last_reset_population;
			witness.residual_run_count = schedule.residual_run_count;
			for (std::size_t index = 0U; index < schedule.residual_run_count; ++index)
			{
				witness.residual_runs[index] = schedule.residual_runs[index];
				witness.residual_runs[index].format = residual_format;
			}
			witness.designated_final_edge = final_edge;
			return witness;
		}

		void apply_migration_boundary(sqlite_descendant_candidate_witness& witness,
									  const std::uint64_t migration_population) noexcept
		{
			for (std::size_t index = 0U; index < witness.residual_run_count; ++index)
			{
				witness.residual_runs[index].format =
					witness.residual_runs[index].population <= migration_population
					? sqlite_compaction_edge_format::legacy_v2
					: sqlite_compaction_edge_format::current_v3;
			}
		}

		struct candidate_edges
		{
			bool legacy_v2_compaction{};
			bool migration{};
			bool v3_compaction{};
			bool v3_compaction_at_or_above_required{};
		};

		void note_v3_population(candidate_edges& edges,
								const std::uint64_t population,
								const std::uint64_t required_population) noexcept
		{
			edges.v3_compaction = true;
			if (required_population != 0U && population >= required_population)
				edges.v3_compaction_at_or_above_required = true;
		}

		[[nodiscard]] candidate_edges
		same_format_edges(const sqlite_authority_format format,
						  const std::uint64_t pre_final_delta,
						  const std::uint64_t lower,
						  const std::uint64_t final_population,
						  const std::uint64_t required_population) noexcept
		{
			candidate_edges edges;
			for_each_inclusive(lower,
							   final_population,
							   [&](const std::uint64_t population)
							   {
								   if (!forced_population_reachable(
										   pre_final_delta, lower, final_population, population))
									   return;
								   if (format == sqlite_authority_format::legacy_v2)
									   edges.legacy_v2_compaction = true;
								   else
									   note_v3_population(edges, population, required_population);
							   });
			if (format == sqlite_authority_format::legacy_v2)
				edges.legacy_v2_compaction = true;
			else
				note_v3_population(edges, final_population, required_population);
			return edges;
		}

		[[nodiscard]] candidate_edges
		migration_last_edges(const std::uint64_t residual,
							 const std::uint64_t lower,
							 const std::uint64_t migration_population) noexcept
		{
			candidate_edges edges;
			edges.migration = true;
			if (migration_population == 0U)
				return edges;
			for_each_inclusive(lower,
							   migration_population,
							   [&](const std::uint64_t population)
							   {
								   if (forced_population_reachable(
										   residual, lower, migration_population, population))
									   edges.legacy_v2_compaction = true;
							   });
			return edges;
		}

		[[nodiscard]] candidate_edges
		migration_final_edges(const std::uint64_t pre_final_delta,
							  const std::uint64_t lower,
							  const std::uint64_t migration_population,
							  const std::uint64_t final_population,
							  const std::uint64_t required_population) noexcept
		{
			candidate_edges edges;
			edges.migration = true;
			const auto legacy_upper = std::min(migration_population, final_population);
			for_each_inclusive(lower,
							   legacy_upper,
							   [&](const std::uint64_t population)
							   {
								   if (forced_population_reachable(
										   pre_final_delta, lower, final_population, population))
									   edges.legacy_v2_compaction = true;
							   });
			const auto v3_lower = std::max(lower, migration_population);
			for_each_inclusive(v3_lower,
							   final_population,
							   [&](const std::uint64_t population)
							   {
								   if (forced_population_reachable(
										   pre_final_delta, lower, final_population, population))
									   note_v3_population(edges, population, required_population);
							   });
			note_v3_population(edges, final_population, required_population);
			return edges;
		}

		enum class candidate_decision : std::uint8_t
		{
			rejected,
			accepted,
			invalid,
		};

		[[nodiscard]] candidate_decision
		validate_candidate(const sqlite_descendant_validation& validation,
						   const sqlite_descendant_candidate_witness& witness) noexcept
		{
			const auto decision =
				validation.candidate_gate(witness, validation.precomputed_projection);
			if (!valid(decision))
				return candidate_decision::invalid;
			return decision == sqlite_candidate_gate_result::accepted
				? candidate_decision::accepted
				: candidate_decision::rejected;
		}

		void accept_candidate(sqlite_authorized_descendant_proof& proof,
							  const sqlite_descendant_candidate_witness& witness,
							  const candidate_edges& edges,
							  const std::uint64_t publish_count) noexcept
		{
			proof.accepted = true;
			proof.added_publish_count = publish_count;
			proof.legacy_v2_compaction_present |= edges.legacy_v2_compaction;
			proof.migration_present |= edges.migration;
			proof.v3_compaction_present |= edges.v3_compaction;
			proof.v3_compaction_at_or_above_required_population |=
				edges.v3_compaction_at_or_above_required;
			if (!proof.canonical_reporting_witness)
				proof.canonical_reporting_witness = witness;
		}

		[[nodiscard]] proof_calculation
		prove_descendant(const sqlite_authority_state_view& source,
						 const sqlite_authority_state_view& target,
						 const sqlite_descendant_validation& validation,
						 const std::uint64_t required_v3_compaction_population) noexcept
		{
			proof_calculation result;
			const bool equal_bytes =
				sqlite_authority_state_bytes_equal(source.canonical_bytes, target.canonical_bytes);
			if (equal_bytes)
			{
				if (!same_summary(source, target))
				{
					result.failure = sqlite_terminal_reclassifier_failure::invalid_equation;
					return result;
				}
				result.proof.accepted = true;
				result.proof.exact_state = true;
				sqlite_descendant_candidate_witness witness;
				witness.candidate_case = sqlite_descendant_candidate_case::same_format_no_reset;
				result.proof.canonical_reporting_witness = witness;
				return result;
			}

			if (!gates_have_valid_enums(validation))
			{
				result.failure = sqlite_terminal_reclassifier_failure::invalid_projection_gate;
				return result;
			}
			if (!common_gates_accepted(validation) ||
				(source.format != target.format &&
				 validation.migration_boundary_commutation != sqlite_projection_gate::accepted))
				return result;
			if (validation.candidate_gate == nullptr)
			{
				result.failure = sqlite_terminal_reclassifier_failure::missing_candidate_gate;
				return result;
			}
			if (target.committed_row_count < source.committed_row_count ||
				(source.format == sqlite_authority_format::current_v3 &&
				 target.format == sqlite_authority_format::legacy_v2))
				return result;

			const auto publish_count = target.committed_row_count - source.committed_row_count;
			const auto source_maximum = equation_origin(source);
			const auto target_maximum = equation_origin(target);
			if (target_maximum < source_maximum)
				return result;
			const auto maximum_delta = target_maximum - source_maximum;
			if (maximum_delta < publish_count)
				return result;
			const auto residual_before_migration = maximum_delta - publish_count;
			const auto lower = std::max<std::uint64_t>(1U, source.committed_row_count);

			bool invalid_candidate_result = false;
			auto consider = [&](const sqlite_descendant_candidate_witness& witness,
								const candidate_edges& edges)
			{
				const auto decision = validate_candidate(validation, witness);
				if (decision == candidate_decision::invalid)
				{
					invalid_candidate_result = true;
					return;
				}
				if (decision == candidate_decision::accepted)
					accept_candidate(result.proof, witness, edges, publish_count);
			};

			if (source.format == target.format)
			{
				if (residual_before_migration == 0U)
				{
					sqlite_descendant_candidate_witness witness;
					witness.candidate_case = sqlite_descendant_candidate_case::same_format_no_reset;
					consider(witness, {});
				}
				const auto last_final_population =
					std::min(target.committed_row_count, residual_before_migration);
				for_each_inclusive(
					lower,
					last_final_population,
					[&](const std::uint64_t final_population)
					{
						if (invalid_candidate_result)
							return;
						const auto pre_final_delta = residual_before_migration - final_population;
						const auto schedule = solve_sqlite_compaction_residual(
							pre_final_delta, lower, final_population);
						if (schedule.status != sqlite_compaction_schedule_status::reachable)
							return;
						const auto edge_format = source.format == sqlite_authority_format::legacy_v2
							? sqlite_compaction_edge_format::legacy_v2
							: sqlite_compaction_edge_format::current_v3;
						const auto final_edge =
							sqlite_compaction_run{edge_format, final_population, 1U};
						const auto witness = make_witness(
							sqlite_descendant_candidate_case::same_format_final_compaction,
							std::nullopt,
							sqlite_descendant_reset_kind::current_format_compaction,
							final_population,
							schedule,
							edge_format,
							final_edge);
						consider(witness,
								 same_format_edges(source.format,
												   pre_final_delta,
												   lower,
												   final_population,
												   required_v3_compaction_population));
					});
			}
			else
			{
				const auto last_migration_population =
					std::min(target.committed_row_count, residual_before_migration);
				for_each_inclusive(
					source.committed_row_count,
					last_migration_population,
					[&](const std::uint64_t migration_population)
					{
						if (invalid_candidate_result)
							return;
						const auto total_residual =
							residual_before_migration - migration_population;
						sqlite_compaction_schedule migration_last_schedule;
						if (migration_population == 0U)
						{
							migration_last_schedule.status = total_residual == 0U
								? sqlite_compaction_schedule_status::reachable
								: sqlite_compaction_schedule_status::unreachable;
						}
						else
						{
							migration_last_schedule = solve_sqlite_compaction_residual(
								total_residual, lower, migration_population);
						}
						if (migration_last_schedule.status ==
							sqlite_compaction_schedule_status::reachable)
						{
							const auto witness = make_witness(
								sqlite_descendant_candidate_case::v2_to_v3_migration_last,
								migration_population,
								sqlite_descendant_reset_kind::migration,
								migration_population,
								migration_last_schedule,
								sqlite_compaction_edge_format::legacy_v2);
							consider(
								witness,
								migration_last_edges(total_residual, lower, migration_population));
						}

						const auto first_final_population =
							std::max<std::uint64_t>(1U, migration_population);
						const auto last_final_population =
							std::min(target.committed_row_count, total_residual);
						for_each_inclusive(
							first_final_population,
							last_final_population,
							[&](const std::uint64_t final_population)
							{
								if (invalid_candidate_result)
									return;
								const auto pre_final_delta = total_residual - final_population;
								auto schedule = solve_sqlite_compaction_residual(
									pre_final_delta, lower, final_population);
								if (schedule.status != sqlite_compaction_schedule_status::reachable)
									return;
								auto witness = make_witness(
									sqlite_descendant_candidate_case::v2_to_v3_final_v3_compaction,
									migration_population,
									sqlite_descendant_reset_kind::current_format_compaction,
									final_population,
									schedule,
									sqlite_compaction_edge_format::legacy_v2,
									sqlite_compaction_run{sqlite_compaction_edge_format::current_v3,
														  final_population,
														  1U});
								apply_migration_boundary(witness, migration_population);
								consider(witness,
										 migration_final_edges(pre_final_delta,
															   lower,
															   migration_population,
															   final_population,
															   required_v3_compaction_population));
							});
					});
			}

			if (invalid_candidate_result)
			{
				result.proof = {};
				result.failure =
					sqlite_terminal_reclassifier_failure::invalid_candidate_gate_result;
			}
			return result;
		}

		[[nodiscard]] bool
		valid_source_for_operation(const sqlite_store_operation operation,
								   const sqlite_authority_state_view& source) noexcept
		{
			switch (operation)
			{
				case sqlite_store_operation::fresh_initialization:
					return source.format == sqlite_authority_format::current_v3 &&
						source.committed_row_count == 0U &&
						source.committed_generation_maximum.tag ==
						sqlite_committed_maximum_tag::none;
				case sqlite_store_operation::publish:
				case sqlite_store_operation::compact_current:
					return source.format == sqlite_authority_format::current_v3;
				case sqlite_store_operation::migrate_predecessor:
					return source.format == sqlite_authority_format::legacy_v2;
				case sqlite_store_operation::wal_recovery_handoff:
					return false;
			}
			return false;
		}
	} // namespace

	bool sqlite_authority_state_bytes_equal(const std::span<const std::byte> left,
											const std::span<const std::byte> right) noexcept
	{
		return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
	}

	sqlite_compaction_schedule solve_sqlite_compaction_residual(
		const std::uint64_t delta,
		const std::uint64_t minimum_population,
		const std::uint64_t maximum_population,
		const std::optional<std::uint64_t> designated_final_population) noexcept
	{
		sqlite_compaction_schedule result;
		result.designated_final_population = designated_final_population;
		if (minimum_population == 0U || maximum_population < minimum_population)
			return result;

		auto remaining = delta;
		if (designated_final_population)
		{
			if (*designated_final_population < minimum_population ||
				*designated_final_population > maximum_population ||
				*designated_final_population > remaining)
			{
				result.status = sqlite_compaction_schedule_status::unreachable;
				return result;
			}
			remaining -= *designated_final_population;
		}
		if (remaining == 0U)
		{
			result.status = sqlite_compaction_schedule_status::reachable;
			return result;
		}

		const auto minimum_run_count = 1U + (remaining - 1U) / maximum_population;
		const auto maximum_run_count = remaining / minimum_population;
		if (minimum_run_count > maximum_run_count)
		{
			result.status = sqlite_compaction_schedule_status::unreachable;
			return result;
		}

		const auto run_count = minimum_run_count;
		const auto base_population = remaining / run_count;
		const auto upper_population_count = remaining % run_count;
		const auto lower_population_count = run_count - upper_population_count;
		if (base_population < minimum_population || base_population > maximum_population ||
			(upper_population_count != 0U && base_population == maximum_population))
		{
			result.status = sqlite_compaction_schedule_status::unreachable;
			return result;
		}

		if (lower_population_count != 0U)
		{
			result.residual_runs[result.residual_run_count++] = {
				sqlite_compaction_edge_format::current_v3, base_population, lower_population_count};
		}
		if (upper_population_count != 0U)
		{
			if (result.residual_run_count >= result.residual_runs.size())
				return result;
			result.residual_runs[result.residual_run_count++] = {
				sqlite_compaction_edge_format::current_v3,
				base_population + 1U,
				upper_population_count};
		}

		std::uint64_t reconstructed{};
		for (std::size_t index = 0U; index < result.residual_run_count; ++index)
		{
			if (!checked_accumulate_product(result.residual_runs[index].population,
											result.residual_runs[index].count,
											reconstructed))
				return result;
		}
		if (reconstructed != remaining)
			return result;
		result.status = sqlite_compaction_schedule_status::reachable;
		return result;
	}

	sqlite_terminal_reclassifier_result
	reclassify_sqlite_terminal(const sqlite_terminal_reclassifier_input& input) noexcept
	{
		sqlite_terminal_reclassifier_result result;
		if (!valid(input.operation))
		{
			result.failure = sqlite_terminal_reclassifier_failure::invalid_operation;
			return result;
		}
		if (!valid(input.main_identity))
		{
			result.failure = sqlite_terminal_reclassifier_failure::invalid_identity_class;
			return result;
		}
		if (!valid(input.terminal.kind) ||
			(input.terminal.kind == sqlite_terminal_observation_kind::valid_authority_state) !=
				input.terminal.state.has_value())
		{
			result.failure = sqlite_terminal_reclassifier_failure::invalid_observation;
			return result;
		}
		if (input.operation != sqlite_store_operation::compact_current &&
			input.required_v3_compaction_population != 0U)
		{
			result.failure = sqlite_terminal_reclassifier_failure::invalid_operation;
			return result;
		}

		if (input.main_identity == sqlite_main_identity_class::unavailable)
			return result;
		if (input.main_identity == sqlite_main_identity_class::changed)
		{
			result.terminal_class = sqlite_terminal_class::valid_non_descendant;
			return result;
		}

		switch (input.terminal.kind)
		{
			case sqlite_terminal_observation_kind::exact_logical_empty_preauthority:
				result.terminal_class =
					input.operation == sqlite_store_operation::fresh_initialization
					? sqlite_terminal_class::exact_logical_empty
					: sqlite_terminal_class::mixed_or_ambiguous;
				return result;
			case sqlite_terminal_observation_kind::invalid_census:
				result.terminal_class = sqlite_terminal_class::invalid_census;
				return result;
			case sqlite_terminal_observation_kind::mixed_or_ambiguous:
				result.terminal_class = sqlite_terminal_class::mixed_or_ambiguous;
				return result;
			case sqlite_terminal_observation_kind::unavailable:
				return result;
			case sqlite_terminal_observation_kind::valid_authority_state:
				break;
		}

		const auto source_validation = validate_state(input.source);
		if (source_validation != state_validation::valid ||
			!valid_source_for_operation(input.operation, input.source))
		{
			result.failure = sqlite_terminal_reclassifier_failure::invalid_source_projection;
			return result;
		}
		if (!gates_have_valid_enums(input.descendant_validation))
		{
			result.failure = sqlite_terminal_reclassifier_failure::invalid_projection_gate;
			return result;
		}

		if (input.expected_candidate)
		{
			if (validate_state(*input.expected_candidate) != state_validation::valid ||
				!valid_candidate_projection(
					input.operation, input.source, *input.expected_candidate))
			{
				result.failure = sqlite_terminal_reclassifier_failure::invalid_candidate_projection;
				return result;
			}
		}
		else if (input.operation == sqlite_store_operation::fresh_initialization)
		{
			result.failure = sqlite_terminal_reclassifier_failure::invalid_candidate_projection;
			return result;
		}

		const auto terminal_validation = validate_state(*input.terminal.state);
		if (terminal_validation == state_validation::invalid_census ||
			terminal_validation == state_validation::invalid_encoding)
		{
			result.terminal_class = sqlite_terminal_class::invalid_census;
			return result;
		}
		if (terminal_validation != state_validation::valid)
		{
			result.failure = sqlite_terminal_reclassifier_failure::invalid_observation;
			return result;
		}

		const auto& terminal = *input.terminal.state;
		result.exact_expected_candidate =
			input.expected_candidate && exact_state(*input.expected_candidate, terminal);
		if (input.expected_candidate &&
			sqlite_authority_state_bytes_equal(input.expected_candidate->canonical_bytes,
											   terminal.canonical_bytes) &&
			!same_summary(*input.expected_candidate, terminal))
		{
			result.terminal_class = sqlite_terminal_class::invalid_census;
			return result;
		}

		const auto calculated = prove_descendant(input.source,
												 terminal,
												 input.descendant_validation,
												 input.required_v3_compaction_population);
		result.proof = calculated.proof;
		if (calculated.failure != sqlite_terminal_reclassifier_failure::none)
		{
			result.failure = calculated.failure;
			return result;
		}
		if (!result.proof.accepted)
		{
			result.terminal_class = sqlite_terminal_class::valid_non_descendant;
			return result;
		}

		switch (input.operation)
		{
			case sqlite_store_operation::fresh_initialization:
				result.required_operation_edge_present = true;
				break;
			case sqlite_store_operation::publish:
				result.required_operation_edge_present =
					result.exact_expected_candidate || result.proof.added_publish_count != 0U;
				break;
			case sqlite_store_operation::migrate_predecessor:
				result.required_operation_edge_present = result.proof.migration_present;
				break;
			case sqlite_store_operation::compact_current:
				result.required_operation_edge_present = result.exact_expected_candidate ||
					result.proof.v3_compaction_at_or_above_required_population;
				break;
			case sqlite_store_operation::wal_recovery_handoff:
				result.failure = sqlite_terminal_reclassifier_failure::invalid_operation;
				return result;
		}

		if (result.required_operation_edge_present)
		{
			result.terminal_class =
				sqlite_terminal_class::authorized_post_state_with_operation_edge;
		}
		else if (result.proof.exact_state)
		{
			result.terminal_class = sqlite_terminal_class::authorized_pre_state;
		}
		else
		{
			result.terminal_class =
				sqlite_terminal_class::authorized_post_state_without_operation_edge;
		}
		return result;
	}
} // namespace cxxlens::sdk
