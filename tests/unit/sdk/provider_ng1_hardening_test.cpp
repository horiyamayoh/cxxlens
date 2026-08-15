#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sdk/provider_ng1_validation_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider::detail;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void require(const result<void>& outcome, const std::string_view message)
	{
		require(outcome.has_value(), message);
	}

	[[nodiscard]] std::string digest(const std::string_view value)
	{
		const auto output = semantic_digest("test.ng1", value);
		require(output.has_value(), "test semantic digest construction failed");
		return *output;
	}

	[[nodiscard]] std::string manifest_digest(const char fill)
	{
		return std::string{"sha256:"} + std::string(64U, fill);
	}

	[[nodiscard]] ng1_session_binding heartbeat_binding()
	{
		return {"provider:test", {1U, 2U, 3U}, "session:test", "task:test", 7U};
	}

	[[nodiscard]] ng1_heartbeat_sample heartbeat_sample(const ng1_heartbeat_kind kind,
														const std::uint64_t sequence,
														const std::uint64_t provider_time,
														const std::uint64_t receipt,
														const std::uint64_t acked = 0U)
	{
		return {"cxxlens.provider-control.heartbeat.v1",
				heartbeat_binding(),
				kind,
				sequence,
				provider_time,
				receipt,
				acked,
				digest("staged")};
	}

	void test_heartbeat_liveness_and_sequence()
	{
		auto state = ng1_heartbeat_state::create(heartbeat_binding(), 1'000U);
		require(state.has_value(), "heartbeat state creation failed");
		require(state->accept(heartbeat_sample(ng1_heartbeat_kind::probe, 0U, 1'000U, 1'000U),
							  0U,
							  digest("staged")),
				"heartbeat probe was rejected");
		require(state->accept(heartbeat_sample(ng1_heartbeat_kind::ack, 0U, 1'001U, 1'001U),
							  0U,
							  digest("staged")),
				"heartbeat ack was rejected");
		require(state->check_liveness(1'000U + 5'000'000'000U - 1U),
				"heartbeat before inclusive deadline was rejected");
		auto timeout = state->check_liveness(1'000U + 5'000'000'000U);
		require(!timeout && timeout.error().code == "provider.heartbeat-timeout",
				"heartbeat inclusive timeout boundary was accepted");

		auto startup = ng1_heartbeat_state::create(heartbeat_binding(), 100U);
		require(startup.has_value(), "startup heartbeat state creation failed");
		require(startup->check_liveness(100U + 10'000'000'000U - 1U),
				"heartbeat startup grace before-boundary was rejected");
		auto startup_timeout = startup->check_liveness(100U + 10'000'000'000U);
		require(!startup_timeout && startup_timeout.error().code == "provider.heartbeat-timeout",
				"heartbeat startup grace boundary was accepted");

		auto duplicate = ng1_heartbeat_state::create(heartbeat_binding(), 0U);
		require(duplicate.has_value(), "duplicate heartbeat state creation failed");
		require(duplicate->accept(
					heartbeat_sample(ng1_heartbeat_kind::probe, 0U, 0U, 0U), 0U, digest("staged")),
				"initial heartbeat probe failed");
		auto duplicate_result = duplicate->accept(
			heartbeat_sample(ng1_heartbeat_kind::probe, 0U, 0U, 0U), 0U, digest("staged"));
		require(!duplicate_result && duplicate_result.error().code == "provider.heartbeat-sequence",
				"duplicate heartbeat sequence was accepted");

		auto unknown_kind = ng1_heartbeat_state::create(heartbeat_binding(), 0U);
		require(unknown_kind.has_value(), "unknown heartbeat kind state creation failed");
		auto unknown_kind_result = unknown_kind->accept(
			heartbeat_sample(static_cast<ng1_heartbeat_kind>(255U), 0U, 0U, 0U),
			0U,
			digest("staged"));
		require(!unknown_kind_result &&
					unknown_kind_result.error().code == "provider.heartbeat-clock-invalid" &&
					unknown_kind_result.error().field == "kind",
				"unknown heartbeat kind was treated as an acknowledgement");
		require(unknown_kind->accept(
					heartbeat_sample(ng1_heartbeat_kind::probe, 0U, 0U, 0U), 0U, digest("staged")),
				"unknown heartbeat kind mutated validator state");

		auto backwards = ng1_heartbeat_state::create(heartbeat_binding(), 0U);
		require(backwards.has_value(), "backwards heartbeat state creation failed");
		require(backwards->accept(heartbeat_sample(ng1_heartbeat_kind::probe, 0U, 10U, 10U),
								  0U,
								  digest("staged")),
				"heartbeat ordering setup failed");
		auto backwards_result = backwards->accept(
			heartbeat_sample(ng1_heartbeat_kind::probe, 1U, 9U, 11U), 0U, digest("staged"));
		require(!backwards_result &&
					backwards_result.error().code == "provider.heartbeat-clock-invalid",
				"backwards provider heartbeat timestamp was accepted");

		auto cross_direction_backwards = ng1_heartbeat_state::create(heartbeat_binding(), 0U);
		require(cross_direction_backwards.has_value(),
				"cross-direction heartbeat state creation failed");
		require(
			cross_direction_backwards->accept(
				heartbeat_sample(ng1_heartbeat_kind::probe, 0U, 10U, 10U), 0U, digest("staged")),
			"cross-direction heartbeat setup failed");
		auto cross_direction_result = cross_direction_backwards->accept(
			heartbeat_sample(ng1_heartbeat_kind::ack, 0U, 9U, 11U), 0U, digest("staged"));
		require(!cross_direction_result &&
					cross_direction_result.error().code == "provider.heartbeat-clock-invalid",
				"cross-direction backwards provider timestamp was accepted");
		require(cross_direction_backwards->accept(
					heartbeat_sample(ng1_heartbeat_kind::ack, 0U, 10U, 11U), 0U, digest("staged")),
				"rejected cross-direction heartbeat mutated clock state");

		auto future = ng1_heartbeat_state::create(heartbeat_binding(), 0U);
		require(future.has_value(), "future heartbeat state creation failed");
		auto future_result = future->accept(
			heartbeat_sample(ng1_heartbeat_kind::probe, 0U, 1U, 0U), 0U, digest("staged"));
		require(!future_result && future_result.error().code == "provider.heartbeat-clock-invalid",
				"future provider heartbeat timestamp was accepted");

		auto initial_deadline = ng1_heartbeat_state::create(heartbeat_binding(), 100U);
		require(initial_deadline.has_value(), "initial ACK deadline state creation failed");
		auto initial_deadline_result = initial_deadline->accept(
			heartbeat_sample(
				ng1_heartbeat_kind::ack, 0U, 100U + 10'000'000'000U, 100U + 10'000'000'000U),
			0U,
			digest("staged"));
		require(!initial_deadline_result &&
					initial_deadline_result.error().code == "provider.heartbeat-timeout",
				"initial ACK at the startup deadline was accepted");

		auto late_ack = ng1_heartbeat_state::create(heartbeat_binding(), 100U);
		require(late_ack.has_value(), "late-ack heartbeat state creation failed");
		require(late_ack->accept(heartbeat_sample(ng1_heartbeat_kind::probe, 0U, 100U, 100U),
								 0U,
								 digest("staged")),
				"late-ack heartbeat probe setup failed");
		auto late_ack_result = late_ack->accept(
			heartbeat_sample(ng1_heartbeat_kind::ack, 0U, 101U, 100U + 5'000'000'000U),
			0U,
			digest("staged"));
		require(!late_ack_result && late_ack_result.error().code == "provider.heartbeat-timeout",
				"ack after the latest probe deadline was accepted");

		auto staged_mismatch = ng1_heartbeat_state::create(heartbeat_binding(), 0U);
		require(staged_mismatch.has_value(), "staged-digest heartbeat state creation failed");
		auto staged_mismatch_result = staged_mismatch->accept(
			heartbeat_sample(ng1_heartbeat_kind::probe, 0U, 0U, 0U), 0U, digest("host-staged"));
		require(!staged_mismatch_result &&
					staged_mismatch_result.error().code == "provider.heartbeat-clock-invalid",
				"heartbeat staged digest not bound to host state");

		auto terminal = ng1_heartbeat_state::create(heartbeat_binding(), 0U);
		require(terminal.has_value() && terminal->mark_terminal(),
				"heartbeat terminal transition failed");
		require(terminal->check_liveness(std::numeric_limits<std::uint64_t>::max()),
				"terminal heartbeat required liveness after completion");
	}

	[[nodiscard]] ng1_progress_sample progress_sample(const std::uint64_t sequence,
													  const std::uint64_t provider_time,
													  const std::uint64_t receipt,
													  const std::uint64_t completed,
													  const std::uint64_t total = 10U)
	{
		return {"cxxlens.provider-control.progress.v2",
				"task:test",
				"dependency:test",
				sequence,
				provider_time,
				receipt,
				completed,
				total};
	}

	void test_progress_rate_and_terminal_boundaries()
	{
		auto future = ng1_progress_state::create("task:test", "dependency:test", 0U);
		require(future.has_value(), "future progress state creation failed");
		auto future_result = future->observe(progress_sample(0U, 1U, 0U, 0U));
		require(!future_result && future_result.error().code == "provider.heartbeat-clock-invalid",
				"future provider progress timestamp was accepted");

		auto initial_deadline = ng1_progress_state::create("task:test", "dependency:test", 0U);
		require(initial_deadline.has_value(), "initial progress deadline state creation failed");
		auto initial_deadline_result =
			initial_deadline->observe(progress_sample(0U, 10'000'000'000U, 10'000'000'000U, 0U));
		require(!initial_deadline_result &&
					initial_deadline_result.error().code == "provider.progress-rate",
				"initial progress sample at the startup deadline was accepted");

		auto state = ng1_progress_state::create("task:test", "dependency:test", 0U);
		require(state.has_value(), "progress state creation failed");
		require(state->observe(progress_sample(0U, 0U, 0U, 0U)), "initial progress sample failed");
		require(state->observe(progress_sample(1U, 1U, 5'000'000'000U, 5U)),
				"progress equality at minimum rate was rejected");
		require(state->observe(progress_sample(2U, 2U, 10'000'000'000U, 5U)),
				"progress exact startup-grace sample was rejected");
		auto zero_after_grace = state->observe(progress_sample(3U, 3U, 15'000'000'000U, 5U));
		require(!zero_after_grace && zero_after_grace.error().code == "provider.progress-rate",
				"zero progress after grace was accepted");

		auto zero_elapsed = ng1_progress_state::create("task:test", "dependency:test", 0U);
		require(zero_elapsed.has_value() && zero_elapsed->observe(progress_sample(0U, 0U, 0U, 0U)),
				"zero-elapsed progress setup failed");
		auto zero_elapsed_result = zero_elapsed->observe(progress_sample(1U, 0U, 0U, 1U));
		require(!zero_elapsed_result &&
					zero_elapsed_result.error().code == "provider.progress-rate",
				"zero-elapsed progress sample was accepted");

		auto short_window = ng1_progress_state::create("task:test", "dependency:test", 0U);
		require(short_window.has_value() && short_window->observe(progress_sample(0U, 0U, 0U, 0U)),
				"short-window progress setup failed");
		require(short_window->observe(progress_sample(1U, 1U, 4'000'000'000U, 4U)),
				"short-window progress sample failed");
		require(short_window->observe(progress_sample(2U, 2U, 8'000'000'000U, 4U)),
				"short-window admission sample failed");
		require(short_window->observe(progress_sample(3U, 3U, 12'000'000'000U, 5U)),
				"short-window post-grace sample failed");
		auto short_window_rate =
			short_window->observe(progress_sample(4U, 4U, 16'000'000'000U, 5U));
		require(!short_window_rate && short_window_rate.error().code == "provider.progress-rate",
				"short-window progress cadence bypassed minimum rate");

		auto large = ng1_progress_state::create("task:test", "dependency:test", 0U);
		require(large.has_value(), "large progress state creation failed");
		require(large->observe(
					progress_sample(0U, 0U, 0U, 0U, std::numeric_limits<std::uint64_t>::max())),
				"large progress initial sample failed");
		require(large->observe(progress_sample(1U,
											   10'000'000'000U,
											   10'000'000'000U,
											   std::numeric_limits<std::uint64_t>::max(),
											   std::numeric_limits<std::uint64_t>::max())),
				"overflow-safe progress comparison rejected a valid fast sample");

		auto gap = ng1_progress_state::create("task:test", "dependency:test", 0U);
		require(gap.has_value(), "gap progress state creation failed");
		require(gap->observe(progress_sample(0U, 0U, 0U, 0U)), "gap initial sample failed");
		auto gap_result = gap->observe(progress_sample(1U, 11'000'000'000U, 11'000'000'000U, 10U));
		require(!gap_result && gap_result.error().code == "provider.progress-rate",
				"maximum progress sample gap was accepted");

		auto terminal = ng1_progress_state::create("task:test", "dependency:test", 0U);
		require(terminal.has_value(), "terminal progress state creation failed");
		require(terminal->observe(progress_sample(0U, 0U, 0U, 0U), false),
				"terminal progress setup failed");
		require(terminal->observe(progress_sample(1U, 1U, 1U, 10U), true),
				"terminal total progress sample failed");
		require(terminal->finish(), "terminal progress finish failed");

		auto incomplete = ng1_progress_state::create("task:test", "dependency:test", 0U);
		require(incomplete.has_value() && incomplete->observe(progress_sample(0U, 0U, 0U, 0U)),
				"incomplete progress setup failed");
		auto finish_result = incomplete->finish();
		require(!finish_result && finish_result.error().code == "provider.progress-rate",
				"incomplete progress finish was accepted");
	}

	[[nodiscard]] ng1_resume_binding resume_binding()
	{
		return {"provider:test",
				{1U, 2U, 3U},
				manifest_digest('a'),
				manifest_digest('b'),
				"session:test",
				"task:test",
				digest("input"),
				digest("invocation"),
				digest("toolchain"),
				digest("environment"),
				digest("sandbox"),
				"dependency:test",
				"atomic:test",
				"batch:test",
				7U};
	}

	[[nodiscard]] ng1_resume_token make_resume_token(const ng1_resume_binding& binding,
													 const std::uint64_t generation = 1U,
													 const std::uint64_t acknowledged_sequence = 4U)
	{
		ng1_resume_token token;
		token.kind = ng1_resume_kind::accepted;
		token.binding = binding;
		token.highest_contiguous_acked_sequence = acknowledged_sequence;
		token.staged_digest = digest("staged");
		token.token_generation = generation;
		const auto token_digest = ng1_resume_token_digest(token);
		require(token_digest.has_value(), "resume token digest construction failed");
		token.token_digest = *token_digest;
		return token;
	}

	[[nodiscard]] ng1_spill_fsync_receipt
	make_fsync_receipt(const ng1_resume_binding& binding,
					   const std::uint64_t acknowledged_sequence = 4U,
					   const std::string staged = digest("staged"),
					   const std::uint64_t fsync_sequence = 1U)
	{
		return {"cxxlens.provider-spill-fsync-receipt.v1",
				binding.provider_id,
				binding.protocol_session_id,
				binding.task_id,
				binding.stream_id,
				acknowledged_sequence,
				staged,
				digest("spill"),
				128U,
				2U,
				fsync_sequence};
	}

	void test_resume_binding_and_projection()
	{
		const auto binding = resume_binding();
		auto unknown_kind = make_resume_token(binding);
		unknown_kind.kind = static_cast<ng1_resume_kind>(255U);
		auto unknown_kind_result = ng1_resume_token_digest(unknown_kind);
		require(!unknown_kind_result &&
					unknown_kind_result.error().code == "provider.resume-replay-invalid",
				"unknown resume kind was accepted");

		auto zero_fsync = make_fsync_receipt(binding, 4U, digest("staged"), 0U);
		auto zero_fsync_result = zero_fsync.validate();
		require(!zero_fsync_result &&
					zero_fsync_result.error().code == "provider.resume-token-stale",
				"zero fsync sequence was accepted");

		auto state = ng1_resume_state::create(binding);
		require(state.has_value(), "resume state creation failed");
		const auto token = make_resume_token(binding);
		require(token.binding.provider_binary_digest == manifest_digest('a'),
				"resume provider binary digest lost manifest grammar");
		require(token.binding.provider_semantic_contract_digest == manifest_digest('b'),
				"resume provider contract digest lost manifest grammar");
		require(token.token_digest.starts_with("semantic-v2:sha256:"),
				"resume token digest left semantic-v2 namespace");
		auto semantic_provider_identity = token;
		semantic_provider_identity.binding.provider_binary_digest = digest("binary");
		auto semantic_provider_identity_result =
			ng1_resume_token_digest(semantic_provider_identity);
		require(!semantic_provider_identity_result &&
					semantic_provider_identity_result.error().code == "provider.resume-token-stale",
				"semantic-v2 provider identity was accepted");
		auto content_digest_in_semantic_field = token;
		content_digest_in_semantic_field.binding.task_input_digest = manifest_digest('c');
		auto content_digest_in_semantic_field_result =
			ng1_resume_token_digest(content_digest_in_semantic_field);
		require(!content_digest_in_semantic_field_result &&
					content_digest_in_semantic_field_result.error().code ==
						"provider.resume-token-stale",
				"manifest content digest was accepted in a semantic field");
		auto changed_provider_identity = binding;
		changed_provider_identity.provider_binary_digest = manifest_digest('d');
		const auto changed_provider_token = make_resume_token(changed_provider_identity);
		require(changed_provider_token.token_digest != token.token_digest,
				"token digest did not bind the exact provider identity string");
		require(state->accept(token, make_fsync_receipt(binding), false, false, 4U),
				"durable resume token was rejected");
		auto start = state->replay_start_sequence();
		require(start && *start == 5U, "resume replay did not start after durable ack");

		auto stale = state->accept(make_resume_token(binding),
								   make_fsync_receipt(binding, 4U, digest("staged"), 2U),
								   false,
								   false,
								   4U);
		require(!stale && stale.error().code == "provider.resume-token-stale",
				"duplicate resume generation was accepted");

		auto rollback = make_resume_token(binding, 2U);
		rollback.highest_contiguous_acked_sequence = 3U;
		const auto rollback_digest = ng1_resume_token_digest(rollback);
		require(rollback_digest.has_value(), "rollback resume digest construction failed");
		rollback.token_digest = *rollback_digest;
		auto rollback_result = state->accept(
			rollback, make_fsync_receipt(binding, 3U, digest("staged"), 2U), false, false, 4U);
		require(!rollback_result &&
					rollback_result.error().code == "provider.resume-replay-invalid",
				"resume acknowledgement rollback was accepted");

		auto foreign_binding = binding;
		foreign_binding.task_id = "task:foreign";
		auto foreign = make_resume_token(foreign_binding, 2U);
		auto foreign_result =
			state->accept(foreign,
						  make_fsync_receipt(foreign_binding, 4U, digest("staged"), 3U),
						  false,
						  false,
						  4U);
		require(!foreign_result && foreign_result.error().code == "provider.resume-token-stale",
				"foreign resume token was accepted");

		auto foreign_group = binding;
		foreign_group.dependency_group_id = "dependency:foreign";
		auto foreign_group_result =
			state->accept(make_resume_token(foreign_group, 2U),
						  make_fsync_receipt(foreign_group, 4U, digest("staged"), 3U),
						  false,
						  false,
						  4U);
		require(!foreign_group_result &&
					foreign_group_result.error().code == "provider.resume-token-stale",
				"foreign dependency group resume token was accepted");

		auto foreign_atomic_group = binding;
		foreign_atomic_group.atomic_output_group_id = "atomic:foreign";
		auto foreign_atomic_group_result =
			state->accept(make_resume_token(foreign_atomic_group, 2U),
						  make_fsync_receipt(foreign_atomic_group, 4U, digest("staged"), 3U),
						  false,
						  false,
						  4U);
		require(!foreign_atomic_group_result &&
					foreign_atomic_group_result.error().code == "provider.resume-token-stale",
				"foreign atomic output group resume token was accepted");

		auto foreign_batch = binding;
		foreign_batch.batch_id = "batch:foreign";
		auto foreign_batch_result =
			state->accept(make_resume_token(foreign_batch, 2U),
						  make_fsync_receipt(foreign_batch, 4U, digest("staged"), 3U),
						  false,
						  false,
						  4U);
		require(!foreign_batch_result &&
					foreign_batch_result.error().code == "provider.resume-token-stale",
				"foreign batch resume token was accepted");

		auto foreign_stream = binding;
		++foreign_stream.stream_id;
		auto foreign_stream_result =
			state->accept(make_resume_token(foreign_stream, 2U),
						  make_fsync_receipt(foreign_stream, 4U, digest("staged"), 3U),
						  false,
						  false,
						  4U);
		require(!foreign_stream_result &&
					foreign_stream_result.error().code == "provider.resume-token-stale",
				"foreign stream resume token was accepted");

		auto mutated = make_resume_token(binding, 2U);
		mutated.staged_digest = digest("mutated");
		auto mutation_result = state->accept(
			mutated, make_fsync_receipt(binding, 4U, digest("staged"), 4U), false, false, 4U);
		require(!mutation_result &&
					mutation_result.error().code == "provider.resume-replay-invalid",
				"mutated resume token without projection refresh was accepted");

		auto volatile_receipt = make_fsync_receipt(binding);
		volatile_receipt.schema = "volatile-ack-without-fsync";
		auto volatile_result =
			state->accept(make_resume_token(binding, 2U), volatile_receipt, false, false, 4U);
		require(!volatile_result && volatile_result.error().code == "provider.resume-token-stale",
				"volatile resume acknowledgement became authority");

		auto open_group_result =
			state->accept(make_resume_token(binding, 2U),
						  make_fsync_receipt(binding, 4U, digest("staged"), 5U),
						  true,
						  false,
						  4U);
		require(!open_group_result &&
					open_group_result.error().code == "provider.resume-token-stale",
				"open dependency group became resume authority");

		auto receipt_replay = state->accept(
			make_resume_token(binding, 2U), make_fsync_receipt(binding), false, false, 4U);
		require(!receipt_replay && receipt_replay.error().code == "provider.resume-replay-invalid",
				"fsync receipt sequence replay was accepted");

		auto ahead = make_resume_token(binding, 2U);
		ahead.highest_contiguous_acked_sequence = 5U;
		const auto ahead_digest = ng1_resume_token_digest(ahead);
		require(ahead_digest.has_value(), "ahead resume digest construction failed");
		ahead.token_digest = *ahead_digest;
		auto ahead_result = state->accept(
			ahead, make_fsync_receipt(binding, 5U, digest("staged"), 6U), false, false, 4U);
		require(!ahead_result && ahead_result.error().code == "provider.resume-replay-invalid",
				"resume ack ahead of observed sequence was accepted");
	}

	[[nodiscard]] ng1_spill_binding spill_binding()
	{
		const auto binding = resume_binding();
		return {binding.provider_id,
				binding.protocol_session_id,
				binding.task_id,
				binding.dependency_group_id,
				binding.atomic_output_group_id,
				binding.batch_id,
				binding.stream_id};
	}

	[[nodiscard]] ng1_spill_record make_spill_record(const ng1_spill_binding& binding,
													 const std::uint64_t ordinal,
													 const std::uint64_t sequence,
													 const std::string_view payload_text)
	{
		ng1_spill_record record;
		record.record_ordinal = ordinal;
		record.task_id = binding.task_id;
		record.dependency_group_id = binding.dependency_group_id;
		record.atomic_output_group_id = binding.atomic_output_group_id;
		record.batch_id = binding.batch_id;
		record.stream_id = binding.stream_id;
		record.sequence = sequence;
		for (const auto byte : payload_text)
			record.payload_bytes.push_back(
				static_cast<std::byte>(static_cast<unsigned char>(byte)));
		auto payload_digest = ng1_spill_payload_digest(record.payload_bytes);
		require(payload_digest.has_value(), "spill payload digest construction failed");
		record.payload_digest = *payload_digest;
		auto record_digest = ng1_spill_record_digest(record);
		require(record_digest.has_value(), "spill record digest construction failed");
		record.record_digest = *record_digest;
		return record;
	}

	void test_spill_prefix_integrity_and_resume_bridge()
	{
		const auto binding = spill_binding();
		auto state = ng1_spill_prefix_state::create(binding);
		require(state.has_value(), "spill prefix state creation failed");

		const auto first = make_spill_record(binding, 0U, 0U, "first");
		require(state->append(first), "valid spill record was rejected");
		require(state->total_records() == 1U && state->total_bytes() > 0U,
				"spill prefix counters were not advanced");
		auto first_prefix_digest = state->spill_digest();
		require(first_prefix_digest.has_value() &&
					first_prefix_digest->starts_with("semantic-v2:sha256:"),
				"spill prefix digest was not semantic-v2 typed");

		auto receipt = state->observe_host_fsync(0U, digest("staged"), 1U);
		require(receipt.has_value(), "host-observed spill fsync receipt construction failed");
		require(receipt->validate(), "host-observed spill fsync receipt was invalid");
		require(receipt->total_records == 1U && receipt->total_bytes == state->total_bytes() &&
					receipt->spill_digest == *first_prefix_digest,
				"spill fsync receipt lost exact prefix counters or digest");

		auto resume = ng1_resume_state::create(resume_binding());
		require(resume.has_value(), "resume state creation for spill bridge failed");
		require(
			resume->accept(make_resume_token(resume_binding(), 1U, 0U), *receipt, false, false, 4U),
			"durable resume did not consume the host-observed spill receipt");
		auto forged_frontier = state->observe_host_fsync(1U, digest("staged"), 2U);
		require(!forged_frontier && forged_frontier.error().code == "provider.spill-corrupt",
				"spill ACK beyond the validated prefix was accepted");

		auto bad_payload = first;
		bad_payload.payload_bytes.push_back(std::byte{'!'});
		auto bad_payload_result = state->append(bad_payload);
		require(!bad_payload_result && bad_payload_result.error().code == "provider.spill-corrupt",
				"payload mutation without digest refresh was accepted");
		require(state->total_records() == 1U, "failed spill append mutated the prefix");

		auto bad_digest = first;
		bad_digest.record_ordinal = 1U;
		bad_digest.sequence = 1U;
		bad_digest.record_digest = digest("not-the-record-digest");
		auto bad_digest_result = state->append(bad_digest);
		require(!bad_digest_result && bad_digest_result.error().code == "provider.spill-corrupt",
				"record digest mutation was accepted");

		auto foreign = make_spill_record(binding, 1U, 1U, "foreign-binding-check");
		foreign.task_id = "task:foreign";
		auto foreign_result = state->append(foreign);
		require(!foreign_result && foreign_result.error().code == "provider.spill-corrupt",
				"foreign spill binding was accepted");

		auto gap = make_spill_record(binding, 2U, 2U, "gap");
		auto gap_result = state->append(gap);
		require(!gap_result && gap_result.error().code == "provider.spill-corrupt",
				"spill ordinal or sequence gap was accepted");

		const auto second = make_spill_record(binding, 1U, 1U, "second");
		require(state->append(second), "valid contiguous spill record was rejected");
		auto second_prefix_digest = state->spill_digest();
		require(second_prefix_digest && *second_prefix_digest != *first_prefix_digest,
				"spill prefix digest ignored append order or content");

		auto invalid_fsync_digest =
			state->observe_host_fsync(4U, "sha256:" + std::string(64U, 'a'), 2U);
		require(!invalid_fsync_digest &&
					invalid_fsync_digest.error().code == "provider.spill-corrupt",
				"legacy spill fsync digest namespace was accepted");
		auto zero_fsync = state->observe_host_fsync(4U, digest("staged"), 0U);
		require(!zero_fsync && zero_fsync.error().code == "provider.spill-corrupt",
				"zero spill fsync sequence was accepted");
	}

	void test_recovery_matrix()
	{
		auto heartbeat = ng1_recovery_transition(ng1_recovery_state::running,
												 ng1_recovery_event::heartbeat_timeout);
		require(heartbeat && *heartbeat == ng1_recovery_state::heartbeat_timeout,
				"heartbeat timeout transition missing");
		auto killed =
			ng1_recovery_transition(*heartbeat, ng1_recovery_event::worker_kill_confirmed);
		require(killed && *killed == ng1_recovery_state::worker_killed,
				"worker kill transition missing");
		auto replay = ng1_recovery_transition(*killed, ng1_recovery_event::durable_token_valid);
		require(replay && *replay == ng1_recovery_state::resume_replay,
				"durable resume transition missing");
		auto resumed = ng1_recovery_transition(*replay, ng1_recovery_event::replay_valid);
		require(resumed && *resumed == ng1_recovery_state::resumed,
				"valid replay transition missing");
		auto completed = ng1_recovery_transition(*resumed, ng1_recovery_event::output_sealed);
		require(completed && *completed == ng1_recovery_state::completed,
				"sealed resumed output transition missing");

		auto invalid =
			ng1_recovery_transition(ng1_recovery_state::completed, ng1_recovery_event::worker_exit);
		require(!invalid && invalid.error().code == "provider.recovery-failed",
				"terminal recovery state accepted an invalid transition");

		auto cancel = ng1_recovery_transition(ng1_recovery_state::running,
											  ng1_recovery_event::cancel_requested);
		require(cancel && *cancel == ng1_recovery_state::cancel_requested,
				"cancellation transition missing");
		auto cancelled = ng1_recovery_transition(*cancel, ng1_recovery_event::cancel_acknowledged);
		require(cancelled && *cancelled == ng1_recovery_state::failed,
				"cancel acknowledgement did not fail closed");
	}
} // namespace

int main()
{
	test_heartbeat_liveness_and_sequence();
	test_progress_rate_and_terminal_boundaries();
	test_resume_binding_and_projection();
	test_spill_prefix_integrity_and_resume_bridge();
	test_recovery_matrix();
	return 0;
}
