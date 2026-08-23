#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>

#include "llvm/clang22/materialization_admission_error.hpp"
#include "llvm/clang22/materialization_io.hpp"
#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/materialization_request_v2_2.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22::materialization;

	[[nodiscard]] sdk::result<json_value> text_value(const std::string_view value)
	{
		return json_value::string(std::string{value});
	}

	[[nodiscard]] sdk::result<json_value>
	object_value(std::initializer_list<std::pair<std::string, json_value>> fields)
	{
		std::map<std::string, json_value, utf8_byte_less> values;
		for (auto&& [key, value] : fields)
			values.emplace(std::move(key), std::move(value));
		return json_value::object(std::move(values));
	}

	enum class signal_state : std::uint8_t
	{
		open,
		cancelled,
	};

	static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
				  "the materializer signal boundary requires a lock-free atomic");
	std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(signal_state::open)};

	void request_cancel(const int) noexcept
	{
		auto expected = static_cast<std::uint8_t>(signal_state::open);
		(void)state.compare_exchange_strong(expected,
											static_cast<std::uint8_t>(signal_state::cancelled),
											std::memory_order_relaxed,
											std::memory_order_relaxed);
	}

	class signal_scope final
	{
	  public:
		signal_scope() noexcept
			: previous_interrupt_{std::signal(SIGINT, request_cancel)},
			  previous_terminate_{std::signal(SIGTERM, request_cancel)}
		{
		}
		signal_scope(const signal_scope&) = delete;
		signal_scope& operator=(const signal_scope&) = delete;
		~signal_scope() noexcept
		{
			if (previous_interrupt_ != SIG_ERR)
				(void)std::signal(SIGINT, previous_interrupt_);
			if (previous_terminate_ != SIG_ERR)
				(void)std::signal(SIGTERM, previous_terminate_);
		}

	  private:
		using handler = void (*)(int);
		handler previous_interrupt_{};
		handler previous_terminate_{};
	};

	class stdin_reader final : public materialization_byte_reader
	{
	  public:
		materialization_io_result<std::size_t> read(const std::span<std::byte> destination) override
		{
			std::cin.read(reinterpret_cast<char*>(destination.data()),
						  static_cast<std::streamsize>(destination.size()));
			const auto received = std::cin.gcount();
			if (std::cin.bad() || (std::cin.fail() && !std::cin.eof()) || received < 0)
				return materialization_io_failure{materialization_io_failure_kind::read,
												  materialization_io_operation::input_read};
			return static_cast<std::size_t>(received);
		}
	};

	struct ingress_envelope
	{
		std::string schema;
		std::string request_version;
		json_value root{json_value::null()};
	};

	[[nodiscard]] sdk::result<ingress_envelope>
	read_ingress_envelope(materialization_replayable_spool& spool)
	{
		constexpr std::size_t maximum_probe_bytes = 16U * 1024U * 1024U;
		if (!spool.sealed() || spool.size_bytes() == 0U || spool.size_bytes() > maximum_probe_bytes)
			return sdk::unexpected(materialization_admission_no_response());
		try
		{
			std::string raw;
			raw.reserve(static_cast<std::size_t>(spool.size_bytes()));
			std::array<std::byte, default_stream_chunk_bytes> buffer{};
			std::uint64_t offset{};
			while (offset < spool.size_bytes())
			{
				const auto remaining = spool.size_bytes() - offset;
				const auto destination = std::span{buffer}.first(
					static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size())));
				auto received = spool.read_at(offset, destination);
				if (!received || *received == 0U || *received > destination.size())
					return sdk::unexpected(materialization_admission_no_response());
				raw.append(reinterpret_cast<const char*>(buffer.data()), *received);
				offset += static_cast<std::uint64_t>(*received);
			}

			json_limits limits;
			limits.max_input_bytes = maximum_probe_bytes;
			auto document = parse_json_object(std::move(raw), limits);
			if (!document)
				return sdk::unexpected(std::move(document.error()));
			const auto* schema = document->root().member("schema");
			const auto* version = document->root().member("request_version");
			if (schema == nullptr || version == nullptr || schema->as_string() == nullptr ||
				version->as_string() == nullptr)
				return sdk::unexpected(
					sdk::error{"materialization.request-invalid", "request-envelope", "missing"});
			return ingress_envelope{*schema->as_string(), *version->as_string(), document->root()};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}

	[[nodiscard]] int no_response() noexcept
	{
		return 2;
	}

	[[nodiscard]] std::string utc_now()
	{
		const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm value{};
#if defined(_WIN32)
		(void)gmtime_s(&value, &time);
#else
		(void)gmtime_r(&time, &value);
#endif
		char output[21]{};
		if (std::strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &value) == 0U)
			return {};
		return output;
	}

	[[nodiscard]] bool write_response(const std::string_view response) noexcept
	{
		try
		{
			std::cout.write(response.data(), static_cast<std::streamsize>(response.size()));
			std::cout.flush();
			return static_cast<bool>(std::cout);
		}
		catch (...)
		{
			return false;
		}
	}

	[[nodiscard]] int emit_failure(const raw_input_observation& input,
								   const std::string_view code,
								   const std::string_view phase,
								   const std::string_view subject,
								   const std::string_view diagnostic)
	{
		auto raw = object_value({
			{"byte_limit", json_value::unsigned_integer(input.byte_limit)},
			{"complete", json_value::boolean(input.complete)},
			{"observed_prefix_digest", text_value(input.observed_prefix_digest).value()},
			{"observed_size_bytes", json_value::unsigned_integer(input.observed_size_bytes)},
		});
		auto binding = object_value({
			{"request", json_value::null()},
			{"state", text_value("raw-input-only").value()},
		});
		auto effects = object_value({
			{"committed_transaction_count", json_value::unsigned_integer(0U)},
			{"head_observation", text_value("not-observed").value()},
			{"observed_head_publication", json_value::null()},
			{"prior_history_retained", json_value::boolean(true)},
			{"publication_attempted", json_value::boolean(false)},
			{"store_draft_state", text_value("not-created").value()},
			{"store_failure_cause", json_value::null()},
			{"task_attempt_count", json_value::unsigned_integer(0U)},
			{"task_success_count", json_value::unsigned_integer(0U)},
			{"worker_launch_attempt_count", json_value::unsigned_integer(0U)},
			{"worker_launch_success_count", json_value::unsigned_integer(0U)},
		});
		auto error = object_value({
			{"code", text_value(code).value()},
			{"diagnostic", text_value(diagnostic).value()},
			{"phase", text_value(phase).value()},
			{"subject", text_value(subject).value()},
		});
		if (!raw || !binding || !effects || !error)
			return no_response();
		auto report = object_value({
			{"binding", std::move(*binding)},
			{"effects", std::move(*effects)},
			{"error", std::move(*error)},
			{"generated_at", text_value(utc_now()).value()},
			{"process_exit_status", json_value::unsigned_integer(1U)},
			{"raw_input_observation", std::move(*raw)},
			{"report_version", text_value("2.2.0").value()},
			{"response_kind", text_value("compact_failure").value()},
			{"result", text_value("failed").value()},
			{"schema", text_value("cxxlens.clang22-materialization-report.v2").value()},
		});
		if (!report)
			return no_response();
		const auto encoded = canonical_json_line(*report);
		return write_response(encoded) ? 1 : no_response();
	}
} // namespace

int main(const int argc, char**)
{
	if (argc != 1)
		return 2;
	signal_scope signals;

	stdin_reader input;
	auto raw_request = make_materialization_private_spool();
	if (!raw_request)
		return no_response();
	auto observed = capture_bounded_input(input, **raw_request);
	if (!observed)
		return no_response();
	if (!observed->complete)
		return emit_failure(*observed,
							"materialization.request-invalid",
							"input-limit",
							"input-limit",
							"maximum-bytes");

	auto ingress = read_ingress_envelope(**raw_request);
	if (!ingress)
	{
		if (is_materialization_admission_no_response(ingress.error()))
			return no_response();
		if (ingress.error().field == "request-envelope")
			return emit_failure(*observed,
								"materialization.request-invalid",
								"request-envelope",
								"request-envelope",
								"missing-or-non-string-envelope:byte=0");
		return emit_failure(*observed,
							"materialization.request-invalid",
							"json-decode",
							"request-envelope",
							"selected-contract");
	}
	if (ingress->schema != materialization_request_v2_2_schema ||
		ingress->request_version != materialization_request_v2_2_version)
		return emit_failure(*observed,
							"materialization.version-unsupported",
							"request-version",
							"request-version",
							"unsupported-version:byte=0");
	if (auto valid = validate_materialization_request_v2_2_document(ingress->root); !valid)
	{
		auto failure = valid.error();
		if (failure.code == "materialization.request-v2_2-invalid")
			failure.code = "materialization.request-invalid";
		return emit_failure(*observed, failure.code, "request-schema", "request", failure.detail);
	}
	return emit_failure(*observed,
						"materialization.request-invalid",
						"request-schema",
						"request-v2_2",
						"source-code=materialization.source-closure-invalid;"
						"source-detail=closure-transport-not-connected");
}
