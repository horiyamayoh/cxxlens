#include "materialization_execution_journal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error journal_error(std::string field, std::string detail)
		{
			return {
				"materialization.execution-journal-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool lower_hex(const std::string_view value) noexcept
		{
			return std::ranges::all_of(value,
									   [](const char character)
									   {
										   return (character >= '0' && character <= '9') ||
											   (character >= 'a' && character <= 'f');
									   });
		}

		[[nodiscard]] bool digest(const std::string_view value, const bool semantic) noexcept
		{
			const auto prefix =
				semantic ? std::string_view{"semantic-v2:sha256:"} : std::string_view{"sha256:"};
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				lower_hex(value.substr(prefix.size()));
		}

		[[nodiscard]] bool store_error_code(const std::string_view value) noexcept
		{
			std::size_t prefix_size{};
			if (value.starts_with("store."))
				prefix_size = std::string_view{"store."}.size();
			else if (value.starts_with("sdk."))
				prefix_size = std::string_view{"sdk."}.size();
			if (prefix_size == 0U || value.size() == prefix_size)
				return false;
			return std::ranges::all_of(value,
									   [](const char character)
									   {
										   return (character >= 'a' && character <= 'z') ||
											   (character >= '0' && character <= '9') ||
											   character == '.' || character == '-';
									   });
		}

		[[nodiscard]] bool bounded_store_detail(const std::string_view value) noexcept
		{
			if (!sdk::validate_utf8_text(value))
				return false;
			std::size_t scalar_count{};
			for (std::size_t index{}; index < value.size(); ++scalar_count)
			{
				const auto first = static_cast<unsigned char>(value[index]);
				index += first <= 0x7fU ? 1U : first <= 0xdfU ? 2U : first <= 0xefU ? 3U : 4U;
			}
			return scalar_count <= 4096U;
		}

		[[nodiscard]] bool phase_code_valid(const compact_failure_phase phase,
											const std::string_view code) noexcept
		{
			switch (phase)
			{
				case compact_failure_phase::input_limit:
				case compact_failure_phase::request_envelope:
					return code == "materialization.request-invalid";
				case compact_failure_phase::json_decode:
				case compact_failure_phase::request_schema:
					return code == "materialization.request-invalid" ||
						code == "materialization.spool-failure";
				case compact_failure_phase::request_version:
					return code == "materialization.version-unsupported";
				case compact_failure_phase::request_binding:
					return code == "materialization.identity-mismatch" ||
						code == "materialization.catalog-census-mismatch" ||
						code == "materialization.task-binding-mismatch" ||
						code == "materialization.descriptor-binding-mismatch" ||
						code == "materialization.spool-failure";
				case compact_failure_phase::installation_binding:
					return code == "materialization.identity-mismatch";
				case compact_failure_phase::worker_launch:
					return code == "materialization.worker-failure";
				case compact_failure_phase::transcript:
					return code == "materialization.transcript-invalid" ||
						code == "materialization.group-incomplete" ||
						code == "materialization.worker-failure";
				case compact_failure_phase::materialization_validation:
					return code == "materialization.span-invalid" ||
						code == "materialization.claim-invalid" ||
						code == "materialization.coverage-incomplete";
				case compact_failure_phase::store_open:
					return code == "materialization.store-failure";
				case compact_failure_phase::store_stage:
					return code == "materialization.store-failure" ||
						code == "materialization.claim-invalid" ||
						code == "materialization.coverage-incomplete";
				case compact_failure_phase::report_construction:
					return code == "materialization.report-invalid";
			}
			return false;
		}

		[[nodiscard]] bool
		is_prepublication_stage_operation(const materialization_store_operation operation) noexcept
		{
			return operation >= materialization_store_operation::head_current &&
				operation <= materialization_store_operation::writer_validate;
		}

		[[nodiscard]] bool untouched_verification_receipts(
			const materialization_store_observation& observation) noexcept
		{
			const std::array expected_paths{
				materialization_store_path::current_selector,
				materialization_store_path::open_publication,
				materialization_store_path::open_snapshot,
			};
			for (std::size_t index{}; index < expected_paths.size(); ++index)
			{
				const auto& receipt = observation.verification_receipts[index];
				if (receipt.path != expected_paths[index] ||
					receipt.status != materialization_store_receipt_status::not_attempted ||
					receipt.id_lookup || receipt.projection || receipt.handle || receipt.error)
					return false;
				if (index == 0U)
				{
					if (receipt.selector_lookup != observation.selector)
						return false;
				}
				else if (receipt.selector_lookup)
					return false;
			}
			return true;
		}
	} // namespace

	struct compact_failure_authority::state
	{
		raw_input_observation input;
		std::optional<compact_request_binding> binding;
		compact_failure_phase phase{compact_failure_phase::input_limit};
		compact_report_error error;
		std::uint64_t worker_launch_attempt_count{};
		std::uint64_t worker_launch_success_count{};
		compact_store_draft_state store_draft_state{compact_store_draft_state::not_created};
		compact_head_observation head_observation{compact_head_observation::not_observed};
		std::optional<std::string> observed_head_publication;
		std::optional<compact_store_failure_observation> store_failure_cause;
	};

	struct materialization_execution_journal::state
	{
		explicit state(raw_input_observation input_value) : input{std::move(input_value)} {}

		[[nodiscard]] sdk::result<void> advance(const compact_failure_phase expected,
												const compact_failure_phase next,
												std::string_view operation)
		{
			if (phase != expected)
				return sdk::unexpected(
					journal_error(std::string{operation}, "out-of-order-phase-transition"));
			phase = next;
			return {};
		}

		raw_input_observation input;
		compact_failure_phase phase{compact_failure_phase::input_limit};
		std::optional<compact_request_binding> binding;
		std::uint64_t actual_task_count{};
		std::uint64_t worker_launch_attempt_count{};
		std::uint64_t worker_launch_success_count{};
		bool worker_launch_in_flight{};
		compact_store_draft_state store_draft_state{compact_store_draft_state::not_created};
		compact_head_observation head_observation{compact_head_observation::not_observed};
		std::optional<std::string> observed_head_publication;
		std::optional<compact_store_failure_observation> store_failure_cause;
		std::optional<materialization_store_preparation> store_preparation;
	};

	compact_failure_authority::compact_failure_authority(std::unique_ptr<state> state) noexcept
		: state_{std::move(state)}
	{
	}

	compact_failure_authority::compact_failure_authority(compact_failure_authority&&) noexcept =
		default;
	compact_failure_authority&
	compact_failure_authority::operator=(compact_failure_authority&&) noexcept = default;
	compact_failure_authority::~compact_failure_authority() = default;

	bool compact_failure_authority::valid() const noexcept
	{
		return static_cast<bool>(state_);
	}

	const raw_input_observation& compact_failure_authority::raw_input() const noexcept
	{
		return state_->input;
	}

	const std::optional<compact_request_binding>&
	compact_failure_authority::request_binding() const noexcept
	{
		return state_->binding;
	}

	compact_failure_phase compact_failure_authority::phase() const noexcept
	{
		return state_->phase;
	}

	const compact_report_error& compact_failure_authority::error() const noexcept
	{
		return state_->error;
	}

	std::uint64_t compact_failure_authority::worker_launch_attempt_count() const noexcept
	{
		return state_->worker_launch_attempt_count;
	}

	std::uint64_t compact_failure_authority::worker_launch_success_count() const noexcept
	{
		return state_->worker_launch_success_count;
	}

	compact_store_draft_state compact_failure_authority::store_draft_state() const noexcept
	{
		return state_->store_draft_state;
	}

	compact_head_observation compact_failure_authority::head_observation() const noexcept
	{
		return state_->head_observation;
	}

	const std::optional<std::string>&
	compact_failure_authority::observed_head_publication() const noexcept
	{
		return state_->observed_head_publication;
	}

	const std::optional<compact_store_failure_observation>&
	compact_failure_authority::store_failure_cause() const noexcept
	{
		return state_->store_failure_cause;
	}

	materialization_postpublication_journal::materialization_postpublication_journal(
		materialization_store_observation observation)
		: observation_{std::move(observation)}
	{
	}

	materialization_postpublication_journal::materialization_postpublication_journal(
		materialization_postpublication_journal&&) noexcept = default;
	materialization_postpublication_journal& materialization_postpublication_journal::operator=(
		materialization_postpublication_journal&&) noexcept = default;
	materialization_postpublication_journal::~materialization_postpublication_journal() = default;

	const materialization_store_observation&
	materialization_postpublication_journal::store_observation() const noexcept
	{
		return observation_;
	}

	materialization_execution_journal::materialization_execution_journal(
		std::unique_ptr<state> state) noexcept
		: state_{std::move(state)}
	{
	}

	materialization_execution_journal::materialization_execution_journal(
		materialization_execution_journal&&) noexcept = default;
	materialization_execution_journal& materialization_execution_journal::operator=(
		materialization_execution_journal&&) noexcept = default;
	materialization_execution_journal::~materialization_execution_journal() = default;

	sdk::result<materialization_execution_journal>
	materialization_execution_journal::begin(raw_input_observation input)
	{
		if (input.byte_limit != maximum_raw_request_bytes ||
			input.observed_size_bytes > maximum_raw_request_bytes + 1U ||
			(input.complete && input.observed_size_bytes > maximum_raw_request_bytes) ||
			!digest(input.observed_prefix_digest, false) ||
			(!input.complete && input.observed_size_bytes != maximum_raw_request_bytes + 1U))
			return sdk::unexpected(journal_error("raw-input", "invalid-observation"));
		return materialization_execution_journal{std::make_unique<state>(std::move(input))};
	}

	sdk::result<void> materialization_execution_journal::pass_input_limit()
	{
		if (!state_)
			return sdk::unexpected(journal_error("input-limit", "consumed-journal"));
		if (!state_->input.complete)
			return sdk::unexpected(journal_error("input-limit", "incomplete-input"));
		return state_->advance(
			compact_failure_phase::input_limit, compact_failure_phase::json_decode, "input-limit");
	}

	sdk::result<void> materialization_execution_journal::pass_json_decode()
	{
		if (!state_)
			return sdk::unexpected(journal_error("json-decode", "consumed-journal"));
		return state_->advance(compact_failure_phase::json_decode,
							   compact_failure_phase::request_envelope,
							   "json-decode");
	}

	sdk::result<void> materialization_execution_journal::pass_request_envelope()
	{
		if (!state_)
			return sdk::unexpected(journal_error("request-envelope", "consumed-journal"));
		return state_->advance(compact_failure_phase::request_envelope,
							   compact_failure_phase::request_version,
							   "request-envelope");
	}

	sdk::result<void> materialization_execution_journal::pass_request_version()
	{
		if (!state_)
			return sdk::unexpected(journal_error("request-version", "consumed-journal"));
		return state_->advance(compact_failure_phase::request_version,
							   compact_failure_phase::request_schema,
							   "request-version");
	}

	sdk::result<void> materialization_execution_journal::pass_request_schema()
	{
		if (!state_)
			return sdk::unexpected(journal_error("request-schema", "consumed-journal"));
		return state_->advance(compact_failure_phase::request_schema,
							   compact_failure_phase::request_binding,
							   "request-schema");
	}

	sdk::result<void>
	materialization_execution_journal::authenticate_request(compact_request_binding binding,
															const std::uint64_t actual_task_count)
	{
		if (!state_)
			return sdk::unexpected(journal_error("request-binding", "consumed-journal"));
		if (state_->phase != compact_failure_phase::request_binding || state_->binding ||
			!state_->input.complete ||
			!sdk::validate_strong_id(binding.materialization_request_id) ||
			!digest(binding.request_digest, true) ||
			!digest(binding.semantic_request_digest, true) || actual_task_count == 0U ||
			actual_task_count > 4096U)
			return sdk::unexpected(
				journal_error("request-binding", "invalid-authenticated-request"));
		state_->binding.emplace(std::move(binding));
		state_->actual_task_count = actual_task_count;
		state_->phase = compact_failure_phase::installation_binding;
		return {};
	}

	sdk::result<void> materialization_execution_journal::complete_installation_binding()
	{
		if (!state_)
			return sdk::unexpected(journal_error("installation-binding", "consumed-journal"));
		if (!state_->binding || state_->actual_task_count == 0U)
			return sdk::unexpected(journal_error("installation-binding", "request-not-bound"));
		return state_->advance(compact_failure_phase::installation_binding,
							   compact_failure_phase::worker_launch,
							   "installation-binding");
	}

	sdk::result<void> materialization_execution_journal::record_worker_launch_attempt()
	{
		if (!state_)
			return sdk::unexpected(journal_error("worker-launch", "consumed-journal"));
		if (state_->phase != compact_failure_phase::worker_launch ||
			state_->worker_launch_in_flight ||
			state_->worker_launch_attempt_count >= state_->actual_task_count)
			return sdk::unexpected(journal_error("worker-launch", "invalid-attempt"));
		++state_->worker_launch_attempt_count;
		state_->worker_launch_in_flight = true;
		return {};
	}

	sdk::result<void> materialization_execution_journal::record_worker_launch_success()
	{
		if (!state_)
			return sdk::unexpected(journal_error("worker-launch", "consumed-journal"));
		if (state_->phase != compact_failure_phase::worker_launch ||
			!state_->worker_launch_in_flight ||
			state_->worker_launch_success_count + 1U != state_->worker_launch_attempt_count)
			return sdk::unexpected(journal_error("worker-launch", "success-without-attempt"));
		++state_->worker_launch_success_count;
		state_->worker_launch_in_flight = false;
		return {};
	}

	sdk::result<void> materialization_execution_journal::complete_worker_launches()
	{
		if (!state_)
			return sdk::unexpected(journal_error("worker-launch", "consumed-journal"));
		if (state_->phase != compact_failure_phase::worker_launch ||
			state_->worker_launch_in_flight ||
			state_->worker_launch_attempt_count != state_->actual_task_count ||
			state_->worker_launch_success_count != state_->actual_task_count)
			return sdk::unexpected(journal_error("worker-launch", "incomplete-task-census"));
		state_->phase = compact_failure_phase::transcript;
		return {};
	}

	sdk::result<void> materialization_execution_journal::complete_transcript_validation()
	{
		if (!state_)
			return sdk::unexpected(journal_error("transcript", "consumed-journal"));
		return state_->advance(compact_failure_phase::transcript,
							   compact_failure_phase::materialization_validation,
							   "transcript");
	}

	sdk::result<void> materialization_execution_journal::complete_materialization_validation()
	{
		if (!state_)
			return sdk::unexpected(journal_error("materialization-validation", "consumed-journal"));
		return state_->advance(compact_failure_phase::materialization_validation,
							   compact_failure_phase::store_open,
							   "materialization-validation");
	}

	sdk::result<void> materialization_execution_journal::record_store_preparation(
		materialization_store_preparation preparation)
	{
		if (!state_)
			return sdk::unexpected(journal_error("store-preparation", "consumed-journal"));
		if (state_->phase != compact_failure_phase::store_open || state_->store_preparation ||
			state_->store_failure_cause)
			return sdk::unexpected(journal_error("store-preparation", "out-of-order-observation"));

		const auto& observation = preparation.observation();
		if (observation.publication_attempted || observation.publish_call_count != 0U ||
			observation.publish_returned_handle || observation.publish_returned_record ||
			observation.recovery_receipt || observation.verification_store ||
			!untouched_verification_receipts(observation) ||
			observation.writer_begin_call_count > 1U)
			return sdk::unexpected(
				journal_error("store-preparation", "postpublication-or-invalid-observation"));

		const auto observe_head = [&]() -> sdk::result<void>
		{
			const auto& head = observation.head_observation;
			if (head.path != materialization_store_path::current_selector)
				return sdk::unexpected(journal_error("store-head", "wrong-access-path"));
			switch (head.status)
			{
				case materialization_store_receipt_status::not_attempted:
					if (head.selector_lookup != observation.selector || head.id_lookup ||
						head.projection || head.handle || head.error)
						return sdk::unexpected(
							journal_error("store-head", "nonempty-not-attempted-receipt"));
					state_->head_observation = compact_head_observation::not_observed;
					return {};
				case materialization_store_receipt_status::present:
					if (head.selector_lookup != observation.selector || head.id_lookup ||
						!head.projection || !head.handle || head.error ||
						!sdk::validate_strong_id(head.projection->publication.publication_id))
						return sdk::unexpected(
							journal_error("store-head", "invalid-present-receipt"));
					state_->head_observation = compact_head_observation::present;
					state_->observed_head_publication = head.projection->publication.publication_id;
					return {};
				case materialization_store_receipt_status::sdk_error:
					if (head.selector_lookup != observation.selector || head.id_lookup ||
						head.projection || head.handle || !head.error)
						return sdk::unexpected(
							journal_error("store-head", "non-authentic-error-receipt"));
					state_->head_observation = head.error->code == "store.current-not-found"
						? compact_head_observation::absent
						: compact_head_observation::sdk_error;
					return {};
			}
			return sdk::unexpected(journal_error("store-head", "unknown-receipt-status"));
		};
		auto head = observe_head();
		if (!head)
			return head;

		if (observation.first_issue)
		{
			const auto* failure =
				std::get_if<materialization_store_sdk_failure>(&*observation.first_issue);
			if (!failure || !store_error_code(failure->error.code) ||
				!sdk::validate_strong_id(failure->error.field) ||
				!bounded_store_detail(failure->error.detail))
				return sdk::unexpected(
					journal_error("store-preparation", "first-issue-is-not-typed-sdk-error"));

			const bool head_failure =
				failure->operation == materialization_store_operation::head_current;
			if (head_failure)
			{
				const auto& head_receipt = observation.head_observation;
				const auto expected_state = failure->error.code == "store.current-not-found"
					? compact_head_observation::absent
					: compact_head_observation::sdk_error;
				if (failure->path != materialization_store_path::current_selector ||
					head_receipt.status != materialization_store_receipt_status::sdk_error ||
					!head_receipt.error || *head_receipt.error != failure->error ||
					state_->head_observation != expected_state ||
					state_->observed_head_publication || observation.writer_begin_call_count != 0U)
					return sdk::unexpected(
						journal_error("store-preparation", "head-cause-receipt-mismatch"));
			}
			else if (failure->path)
				return sdk::unexpected(
					journal_error("store-preparation", "non-head-cause-has-access-path"));

			const auto detail = std::as_bytes(
				std::span<const char>{failure->error.detail.data(), failure->error.detail.size()});
			state_->store_failure_cause = compact_store_failure_observation{
				failure->operation,
				failure->path,
				failure->error.code,
				failure->error.field,
				failure->error.detail,
				static_cast<std::uint64_t>(detail.size()),
				sdk::content_digest(detail),
			};

			if (failure->operation == materialization_store_operation::store_open)
			{
				if (state_->head_observation != compact_head_observation::not_observed ||
					failure->path || observation.writer_begin_call_count != 0U)
					return sdk::unexpected(
						journal_error("store-preparation", "invalid-store-open-failure"));
				state_->store_draft_state = compact_store_draft_state::not_created;
				return {};
			}

			if (!is_prepublication_stage_operation(failure->operation) ||
				state_->head_observation == compact_head_observation::not_observed ||
				(!head_failure && state_->head_observation == compact_head_observation::sdk_error))
				return sdk::unexpected(
					journal_error("store-preparation", "unrepresentable-store-stage-failure"));
			state_->phase = compact_failure_phase::store_stage;
			state_->store_draft_state = compact_store_draft_state::discarded;
			return {};
		}

		if (!preparation.ready_for_publish() ||
			state_->head_observation == compact_head_observation::not_observed ||
			observation.writer_begin_call_count != 1U)
			return sdk::unexpected(journal_error("store-preparation", "not-ready-without-cause"));
		state_->phase = compact_failure_phase::store_stage;
		state_->store_preparation.emplace(std::move(preparation));
		return {};
	}

	sdk::result<void> materialization_execution_journal::complete_store_preparation()
	{
		if (!state_)
			return sdk::unexpected(journal_error("store-stage", "consumed-journal"));
		if (state_->phase != compact_failure_phase::store_stage || !state_->store_preparation ||
			!state_->store_preparation->ready_for_publish() || state_->store_failure_cause ||
			state_->head_observation == compact_head_observation::not_observed)
			return sdk::unexpected(journal_error("store-stage", "preparation-not-ready"));
		state_->phase = compact_failure_phase::report_construction;
		return {};
	}

	sdk::result<compact_failure_authority>
	materialization_execution_journal::issue_compact_failure(compact_report_error error) &&
	{
		if (!state_)
			return sdk::unexpected(journal_error("compact-failure", "consumed-journal"));
		if (!phase_code_valid(state_->phase, error.code) ||
			!sdk::validate_strong_id(error.subject) || error.diagnostic.empty() ||
			error.diagnostic.size() > 4096U || error.diagnostic.contains('\0') ||
			!sdk::validate_utf8_text(error.diagnostic))
			return sdk::unexpected(journal_error("compact-failure", "invalid-error"));

		const bool raw_phase = state_->phase <= compact_failure_phase::request_binding;
		if (raw_phase != !state_->binding ||
			((state_->phase == compact_failure_phase::input_limit) == state_->input.complete))
			return sdk::unexpected(journal_error("compact-failure", "binding-or-input-matrix"));

		if (state_->phase == compact_failure_phase::worker_launch)
		{
			if (!state_->worker_launch_in_flight || state_->worker_launch_attempt_count == 0U ||
				state_->worker_launch_attempt_count > state_->actual_task_count ||
				state_->worker_launch_success_count + 1U != state_->worker_launch_attempt_count)
				return sdk::unexpected(journal_error("compact-failure", "launch-census"));
		}
		else if (state_->phase >= compact_failure_phase::transcript)
		{
			if (state_->worker_launch_in_flight ||
				state_->worker_launch_attempt_count != state_->actual_task_count ||
				state_->worker_launch_success_count != state_->actual_task_count)
				return sdk::unexpected(journal_error("compact-failure", "launch-census"));
		}
		else if (state_->worker_launch_attempt_count != 0U ||
				 state_->worker_launch_success_count != 0U || state_->worker_launch_in_flight)
			return sdk::unexpected(journal_error("compact-failure", "early-launch-census"));

		if (state_->phase == compact_failure_phase::store_stage ||
			state_->phase == compact_failure_phase::report_construction)
		{
			if (state_->head_observation == compact_head_observation::not_observed)
				return sdk::unexpected(journal_error("compact-failure", "head-not-observed"));
			// Releasing the move-only preparation discards the unpublished SDK writer before the
			// authority claiming zero committed transactions becomes observable by the caller.
			state_->store_preparation.reset();
			state_->store_draft_state = compact_store_draft_state::discarded;
		}
		else if (state_->head_observation != compact_head_observation::not_observed ||
				 state_->observed_head_publication ||
				 state_->store_draft_state != compact_store_draft_state::not_created)
			return sdk::unexpected(journal_error("compact-failure", "early-store-effect"));

		const bool store_failure = error.code == "materialization.store-failure" &&
			(state_->phase == compact_failure_phase::store_open ||
			 state_->phase == compact_failure_phase::store_stage);
		if (store_failure != state_->store_failure_cause.has_value())
			return sdk::unexpected(journal_error("compact-failure", "store-cause-matrix"));

		auto authority = std::make_unique<compact_failure_authority::state>();
		authority->input = std::move(state_->input);
		authority->binding = std::move(state_->binding);
		authority->phase = state_->phase;
		authority->error = std::move(error);
		authority->worker_launch_attempt_count = state_->worker_launch_attempt_count;
		authority->worker_launch_success_count = state_->worker_launch_success_count;
		authority->store_draft_state = state_->store_draft_state;
		authority->head_observation = state_->head_observation;
		authority->observed_head_publication = std::move(state_->observed_head_publication);
		authority->store_failure_cause = std::move(state_->store_failure_cause);
		state_.reset();
		return compact_failure_authority{std::move(authority)};
	}

	sdk::result<materialization_postpublication_journal>
	materialization_execution_journal::begin_publication() &&
	{
		if (!state_)
			return sdk::unexpected(journal_error("publication", "consumed-journal"));
		if (state_->phase != compact_failure_phase::report_construction ||
			!state_->store_preparation || !state_->store_preparation->ready_for_publish() ||
			state_->store_failure_cause)
			return sdk::unexpected(journal_error("publication", "prepublication-not-ready"));

		auto preparation = std::move(*state_->store_preparation);
		state_.reset();
		return materialization_postpublication_journal{
			publish_materialization_store(std::move(preparation))};
	}
} // namespace cxxlens::detail::clang22::materialization
