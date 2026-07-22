#include "materialization_report.hpp"

#include <chrono>
#include <map>
#include <string_view>
#include <utility>

#include "materialization_json.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {"materialization.report-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool generated_at(const std::string_view value) noexcept
		{
			// The production clock emits this closed UTC spelling. Restricting the accepted form is
			// stronger than the schema's date-time format and keeps report bytes deterministic.
			if (value.size() != 20U || value[4U] != '-' || value[7U] != '-' || value[10U] != 'T' ||
				value[13U] != ':' || value[16U] != ':' || value[19U] != 'Z')
				return false;
			for (const auto index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U})
				if (value[index] < '0' || value[index] > '9')
					return false;
			const auto number = [&](const std::size_t offset, const std::size_t count)
			{
				unsigned result{};
				for (std::size_t index{}; index < count; ++index)
					result = result * 10U + static_cast<unsigned>(value[offset + index] - '0');
				return result;
			};
			const std::chrono::year_month_day date{
				std::chrono::year{static_cast<int>(number(0U, 4U))},
				std::chrono::month{number(5U, 2U)},
				std::chrono::day{number(8U, 2U)}};
			return date.ok() && number(11U, 2U) <= 23U && number(14U, 2U) <= 59U &&
				number(17U, 2U) <= 59U;
		}

		[[nodiscard]] sdk::result<json_value> string(std::string value)
		{
			auto result = json_value::string(std::move(value));
			if (!result)
				return sdk::unexpected(invalid("report", "invalid-string"));
			return result;
		}

		[[nodiscard]] sdk::result<json_value>
		object(std::initializer_list<std::pair<std::string, json_value>> fields)
		{
			json_value::object_type value;
			for (auto& [name, member] : fields)
				if (!value.emplace(name, std::move(member)).second)
					return sdk::unexpected(invalid("report", "duplicate-member"));
			auto result = json_value::object(std::move(value));
			if (!result)
				return sdk::unexpected(invalid("report", "invalid-object"));
			return result;
		}

		[[nodiscard]] std::string_view text(const compact_store_draft_state value) noexcept
		{
			return value == compact_store_draft_state::not_created ? "not-created" : "discarded";
		}

		[[nodiscard]] std::string_view text(const compact_head_observation value) noexcept
		{
			switch (value)
			{
				case compact_head_observation::not_observed:
					return "not-observed";
				case compact_head_observation::absent:
					return "absent";
				case compact_head_observation::present:
					return "present";
				case compact_head_observation::sdk_error:
					return "sdk-error";
			}
			return {};
		}

		[[nodiscard]] std::string_view text(const materialization_store_operation value) noexcept
		{
			switch (value)
			{
				case materialization_store_operation::configuration:
					return "configuration";
				case materialization_store_operation::store_open:
					return "store_open";
				case materialization_store_operation::head_current:
					return "head_current";
				case materialization_store_operation::writer_begin:
					return "writer_begin";
				case materialization_store_operation::partition_stage:
					return "partition_stage";
				case materialization_store_operation::closure_stage:
					return "closure_stage";
				case materialization_store_operation::writer_validate:
					return "writer_validate";
				case materialization_store_operation::writer_publish:
					return "writer_publish";
				case materialization_store_operation::store_reopen:
					return "store_reopen";
				case materialization_store_operation::verify_current:
					return "verify_current";
				case materialization_store_operation::verify_open_publication:
					return "verify_open_publication";
				case materialization_store_operation::verify_open_snapshot:
					return "verify_open_snapshot";
				case materialization_store_operation::verify_projection:
					return "verify_projection";
			}
			return {};
		}

		[[nodiscard]] std::string_view text(const compact_failure_phase value) noexcept
		{
			switch (value)
			{
				case compact_failure_phase::input_limit:
					return "input-limit";
				case compact_failure_phase::json_decode:
					return "json-decode";
				case compact_failure_phase::request_envelope:
					return "request-envelope";
				case compact_failure_phase::request_version:
					return "request-version";
				case compact_failure_phase::request_schema:
					return "request-schema";
				case compact_failure_phase::request_binding:
					return "request-binding";
				case compact_failure_phase::installation_binding:
					return "installation-binding";
				case compact_failure_phase::worker_launch:
					return "worker-launch";
				case compact_failure_phase::transcript:
					return "transcript";
				case compact_failure_phase::materialization_validation:
					return "materialization-validation";
				case compact_failure_phase::store_open:
					return "store-open";
				case compact_failure_phase::store_stage:
					return "store-stage";
				case compact_failure_phase::report_construction:
					return "report-construction";
			}
			return {};
		}

		[[nodiscard]] std::string_view text(const materialization_store_path value) noexcept
		{
			switch (value)
			{
				case materialization_store_path::current_selector:
					return "current-selector";
				case materialization_store_path::open_publication:
					return "open-publication";
				case materialization_store_path::open_snapshot:
					return "open-snapshot";
			}
			return {};
		}
	} // namespace

	sdk::result<std::string>
	encode_compact_failure_report(const compact_failure_authority& authority, std::string timestamp)
	{
		if (!authority.valid() || !generated_at(timestamp))
			return sdk::unexpected(invalid("report", "compact-authority-or-time"));

		const auto& input = authority.raw_input();
		const auto& binding = authority.request_binding();
		const auto& error = authority.error();

		auto schema = string("cxxlens.clang22-materialization-report.v2");
		auto report_version = string("2.1.0");
		auto response_kind = string("compact_failure");
		auto result = string("failed");
		auto generated = string(std::move(timestamp));
		auto prefix_digest = string(input.observed_prefix_digest);
		auto binding_state = string(binding ? "request-bound" : "raw-input-only");
		auto store_state = string(std::string{text(authority.store_draft_state())});
		auto head_state = string(std::string{text(authority.head_observation())});
		auto code = string(error.code);
		auto phase = string(std::string{text(authority.phase())});
		auto subject = string(error.subject);
		auto diagnostic = string(error.diagnostic);
		if (!schema || !report_version || !response_kind || !result || !generated ||
			!prefix_digest || !binding_state || !store_state || !head_state || !code || !phase ||
			!subject || !diagnostic)
			return sdk::unexpected(invalid("report", "string"));

		auto raw = object({
			{"byte_limit", json_value::unsigned_integer(input.byte_limit)},
			{"complete", json_value::boolean(input.complete)},
			{"observed_prefix_digest", std::move(*prefix_digest)},
			{"observed_size_bytes", json_value::unsigned_integer(input.observed_size_bytes)},
		});
		json_value request = json_value::null();
		if (binding)
		{
			auto id = string(binding->materialization_request_id);
			auto request_digest = string(binding->request_digest);
			auto semantic_digest = string(binding->semantic_request_digest);
			if (!id || !request_digest || !semantic_digest)
				return sdk::unexpected(invalid("report.binding", "string"));
			auto value = object({
				{"materialization_request_id", std::move(*id)},
				{"request_digest", std::move(*request_digest)},
				{"semantic_request_digest", std::move(*semantic_digest)},
			});
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			request = std::move(*value);
		}
		auto binding_value = object({
			{"request", std::move(request)},
			{"state", std::move(*binding_state)},
		});
		json_value observed_head = json_value::null();
		if (authority.observed_head_publication())
		{
			auto value = string(*authority.observed_head_publication());
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			observed_head = std::move(*value);
		}
		json_value store_cause = json_value::null();
		if (authority.store_failure_cause())
		{
			const auto& cause = *authority.store_failure_cause();
			auto kind = string("sdk_error");
			auto operation = string(std::string{text(cause.operation)});
			auto cause_code = string(cause.code);
			auto field = string(cause.field);
			if (!kind || !operation || !cause_code || !field)
				return sdk::unexpected(invalid("report.effects", "store-cause-string"));
			auto detail_kind = string("opaque");
			auto detail_digest = string(cause.detail_digest);
			auto detail_diagnostic = string(cause.detail);
			if (!detail_kind || !detail_digest || !detail_diagnostic)
				return sdk::unexpected(invalid("report.effects", "store-detail-string"));
			auto detail = object({
				{"byte_count", json_value::unsigned_integer(cause.detail_byte_count)},
				{"diagnostic", std::move(*detail_diagnostic)},
				{"digest", std::move(*detail_digest)},
				{"kind", std::move(*detail_kind)},
			});
			if (!detail)
				return sdk::unexpected(std::move(detail.error()));
			json_value access_path = json_value::null();
			if (cause.path)
			{
				auto path = string(std::string{text(*cause.path)});
				if (!path)
					return sdk::unexpected(invalid("report.effects", "store-path-string"));
				access_path = std::move(*path);
			}
			auto value = object({
				{"access_path", std::move(access_path)},
				{"code", std::move(*cause_code)},
				{"detail", std::move(*detail)},
				{"field", std::move(*field)},
				{"kind", std::move(*kind)},
				{"operation", std::move(*operation)},
			});
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			store_cause = std::move(*value);
		}
		auto effect_value = object({
			{"committed_transaction_count", json_value::unsigned_integer(0U)},
			{"head_observation", std::move(*head_state)},
			{"observed_head_publication", std::move(observed_head)},
			{"prior_history_retained", json_value::boolean(true)},
			{"publication_attempted", json_value::boolean(false)},
			{"store_draft_state", std::move(*store_state)},
			{"store_failure_cause", std::move(store_cause)},
			{"worker_launch_attempt_count",
			 json_value::unsigned_integer(authority.worker_launch_attempt_count())},
			{"worker_launch_success_count",
			 json_value::unsigned_integer(authority.worker_launch_success_count())},
		});
		auto error_value = object({
			{"code", std::move(*code)},
			{"diagnostic", std::move(*diagnostic)},
			{"phase", std::move(*phase)},
			{"subject", std::move(*subject)},
		});
		if (!raw || !binding_value || !effect_value || !error_value)
			return sdk::unexpected(invalid("report", "object"));
		auto root = object({
			{"binding", std::move(*binding_value)},
			{"effects", std::move(*effect_value)},
			{"error", std::move(*error_value)},
			{"generated_at", std::move(*generated)},
			{"process_exit_status", json_value::unsigned_integer(1U)},
			{"raw_input_observation", std::move(*raw)},
			{"report_version", std::move(*report_version)},
			{"response_kind", std::move(*response_kind)},
			{"result", std::move(*result)},
			{"schema", std::move(*schema)},
		});
		if (!root)
			return sdk::unexpected(std::move(root.error()));
		return canonical_json_line(*root);
	}
} // namespace cxxlens::detail::clang22::materialization
