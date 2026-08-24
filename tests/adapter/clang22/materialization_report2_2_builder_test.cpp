#include "llvm/clang22/materialization_report2_2_builder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	struct spool_observation
	{
		std::uint64_t append_calls{};
		std::uint64_t seal_calls{};
		std::uint64_t rewind_calls{};
		std::uint64_t read_calls{};
		std::uint64_t reserve_calls{};
	};

	class memory_spool final : public materialization_report2_2_reserved_spool
	{
	  public:
		explicit memory_spool(std::shared_ptr<spool_observation> observation,
							  const std::uint64_t maximum_reservable_terminal = 512U)
			: observation_{std::move(observation)},
			  maximum_reservable_terminal_{maximum_reservable_terminal}
		{
		}

		materialization_io_result<void> append(const std::span<const std::byte> bytes) override
		{
			++observation_->append_calls;
			if (sealed_)
				return materialization_io_failure{
					materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::spool_write};
			bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
			return {};
		}

		materialization_io_result<void> seal() override
		{
			++observation_->seal_calls;
			sealed_ = true;
			return {};
		}

		materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
		{
			auto result = read_at(cursor_, destination);
			if (result)
				cursor_ += *result;
			return result;
		}

		materialization_io_result<std::size_t> read_at(const std::uint64_t offset,
													   std::span<std::byte> destination) override
		{
			++observation_->read_calls;
			if (offset > bytes_.size())
				return materialization_io_failure{
					materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::spool_read};
			const auto count = std::min<std::size_t>(
				bytes_.size() - static_cast<std::size_t>(offset), destination.size());
			std::ranges::copy(std::span{bytes_}.subspan(static_cast<std::size_t>(offset), count),
							  destination.begin());
			return count;
		}

		materialization_io_result<void> rewind() override
		{
			++observation_->rewind_calls;
			cursor_ = 0U;
			return {};
		}

		[[nodiscard]] std::uint64_t size_bytes() const noexcept override
		{
			return bytes_.size();
		}

		[[nodiscard]] bool sealed() const noexcept override
		{
			return sealed_;
		}
		materialization_io_result<void> reserve_terminal_bytes(const std::uint64_t count) override
		{
			++observation_->reserve_calls;
			if (count > maximum_reservable_terminal_)
				return materialization_io_failure{materialization_io_failure_kind::spool,
												  materialization_io_operation::spool_write};
			reserved_terminal_ = count;
			return {};
		}
		[[nodiscard]] std::uint64_t reserved_terminal_bytes() const noexcept override
		{
			return reserved_terminal_;
		}

	  private:
		std::shared_ptr<spool_observation> observation_;
		std::vector<std::byte> bytes_;
		std::uint64_t cursor_{};
		bool sealed_{};
		std::uint64_t maximum_reservable_terminal_{};
		std::uint64_t reserved_terminal_{};
	};

	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text)
	{
		std::vector<std::byte> result;
		result.reserve(text.size());
		for (const auto value : text)
			result.push_back(static_cast<std::byte>(value));
		return result;
	}

	class projection_port final : public materialization_report2_2_projection_port
	{
	  public:
		std::string prefix{"{\"pre\":"};
		std::string suffix{"true}"};
		std::uint64_t prepublication_calls{};
		std::uint64_t terminal_calls{};
		std::uint64_t validation_calls{};
		bool reject_sealed{};
		materialization_report2_2_prepublication_projection observed_pre;
		materialization_report2_2_terminal_projection observed_terminal;

		cxxlens::sdk::result<void>
		write_prepublication(const materialization_report2_2_prepublication_projection& projection,
							 materialization_report2_2_chunk_sink& sink,
							 std::stop_token) override
		{
			++prepublication_calls;
			observed_pre = projection;
			const auto value = bytes(prefix);
			return sink.append(value);
		}

		cxxlens::sdk::result<void>
		write_terminal(const materialization_report2_2_terminal_projection& projection,
					   materialization_report2_2_chunk_sink& sink) override
		{
			++terminal_calls;
			observed_terminal = projection;
			const auto value = bytes(suffix);
			return sink.append(value);
		}

		cxxlens::sdk::result<void>
		validate_sealed(const materialization_report2_2_prepublication_projection& prepublication,
						const materialization_report2_2_terminal_projection& terminal,
						materialization_replayable_spool& report,
						const materialization_report2_2_receipt& receipt) override
		{
			++validation_calls;
			if (reject_sealed)
				return cxxlens::sdk::unexpected(
					cxxlens::sdk::error{"test.report-invalid", "sealed", "injected"});
			if (prepublication != observed_pre || terminal != observed_terminal ||
				!report.sealed() || receipt.byte_count != prefix.size() + suffix.size() ||
				receipt.terminal != terminal.terminal)
				return cxxlens::sdk::unexpected(
					cxxlens::sdk::error{"test.report-invalid", "projection", "mismatch"});
			return {};
		}
	};

	[[nodiscard]] materialization_report2_2_prepublication_projection prepublication()
	{
		return {
			"materialization-request:test",
			"semantic-v2:sha256:request",
			"semantic-v2:sha256:semantic-request",
			"semantic-v2:sha256:request-authority",
			2U,
			1U,
			4096U,
			"semantic-v2:sha256:closure-receipt-set",
			"semantic-v2:sha256:worker-result-set",
			"semantic-v2:sha256:base-row-result-set",
			"semantic-v2:sha256:task-receipt",
			"semantic-v2:sha256:store-source",
			"semantic-v2:sha256:store-preparation",
			"semantic-v2:sha256:store-projection",
			"semantic-v2:sha256:store-projection",
			"semantic-v2:sha256:journal",
		};
	}

	[[nodiscard]] materialization_report2_2_terminal_projection terminal()
	{
		return {
			materialization_report2_2_store_terminal::committed_verified,
			"semantic-v2:sha256:store-preparation",
			"semantic-v2:sha256:store-result",
			"publication:test",
			1U,
		};
	}

	void positive_two_phase_stream()
	{
		auto observation = std::make_shared<spool_observation>();
		projection_port port;
		auto prepared = materialization_report2_2_builder::prepare(
			std::make_unique<memory_spool>(observation), prepublication(), port, {1024U, 256U});
		require(prepared.has_value(), "prepublication phase was rejected");
		require(prepared->terminal_space_reserved() && prepared->prepublication_bytes() != 0U,
				"terminal budget was not reserved before publication");
		require(observation->seal_calls == 0U && observation->reserve_calls == 1U &&
					port.terminal_calls == 0U,
				"prepublication phase crossed the report terminal");

		auto sealed = std::move(*prepared).finalize(terminal());
		require(sealed.has_value(), "terminal report phase was rejected");
		require(sealed->receipt().terminal ==
						materialization_report2_2_store_terminal::committed_verified &&
					sealed->receipt().byte_count == port.prefix.size() + port.suffix.size() &&
					!sealed->receipt().content_digest.empty() &&
					!sealed->receipt().semantic_digest.empty(),
				"sealed report receipt lost terminal or byte identity");
		require(observation->append_calls == 2U && observation->seal_calls == 1U &&
					observation->rewind_calls >= 1U && port.prepublication_calls == 1U &&
					port.terminal_calls == 1U && port.validation_calls == 1U,
				"two-phase report operations were not exact");
	}

	void expected_actual_mismatch_is_zero_write()
	{
		auto observation = std::make_shared<spool_observation>();
		projection_port port;
		auto projection = prepublication();
		projection.actual_projection_digest = "semantic-v2:sha256:different";
		auto prepared =
			materialization_report2_2_builder::prepare(std::make_unique<memory_spool>(observation),
													   std::move(projection),
													   port,
													   {1024U, 256U});
		require(!prepared && observation->append_calls == 0U && port.prepublication_calls == 0U,
				"mismatched Store projection wrote report bytes");
	}

	void phase_limits_fail_closed()
	{
		auto observation = std::make_shared<spool_observation>();
		projection_port port;
		port.prefix = "12345";
		auto prefix_overflow = materialization_report2_2_builder::prepare(
			std::make_unique<memory_spool>(observation), prepublication(), port, {8U, 4U});
		require(!prefix_overflow && observation->seal_calls == 0U,
				"oversized prefix crossed the report boundary");

		observation = std::make_shared<spool_observation>();
		projection_port terminal_port;
		terminal_port.prefix = "123";
		terminal_port.suffix = "12345";
		auto prepared =
			materialization_report2_2_builder::prepare(std::make_unique<memory_spool>(observation),
													   prepublication(),
													   terminal_port,
													   {32U, 4U});
		require(prepared.has_value(), "bounded prefix was rejected");
		auto terminal_overflow = std::move(*prepared).finalize(terminal());
		require(!terminal_overflow && observation->seal_calls == 0U &&
					terminal_port.validation_calls == 0U,
				"oversized terminal was sealed or validated");
	}

	void terminal_must_bind_preparation()
	{
		auto observation = std::make_shared<spool_observation>();
		projection_port port;
		auto prepared = materialization_report2_2_builder::prepare(
			std::make_unique<memory_spool>(observation), prepublication(), port, {1024U, 256U});
		require(prepared.has_value(), "preparation-binding fixture failed");
		auto value = terminal();
		value.store_preparation_digest = "semantic-v2:sha256:wrong-preparation";
		auto rejected = std::move(*prepared).finalize(std::move(value));
		require(!rejected && port.terminal_calls == 0U && observation->seal_calls == 0U,
				"wrong Store preparation reached terminal encoding");
	}

	void physical_reservation_precedes_encoding()
	{
		auto observation = std::make_shared<spool_observation>();
		projection_port port;
		auto no_terminal_reservation = materialization_report2_2_builder::prepare(
			std::make_unique<memory_spool>(observation, 255U),
			prepublication(),
			port,
			{1024U, 256U});
		require(!no_terminal_reservation && port.prepublication_calls == 1U &&
					observation->append_calls == 1U && observation->reserve_calls == 1U &&
					observation->seal_calls == 0U,
				"missing terminal reservation did not fail before publication");
	}

	void cancellation_precedes_encoding()
	{
		auto observation = std::make_shared<spool_observation>();
		projection_port port;
		std::stop_source cancellation;
		cancellation.request_stop();
		auto cancelled =
			materialization_report2_2_builder::prepare(std::make_unique<memory_spool>(observation),
													   prepublication(),
													   port,
													   {1024U, 256U},
													   cancellation.get_token());
		require(!cancelled && port.prepublication_calls == 0U && observation->append_calls == 0U,
				"cancelled report reached prepublication encoding");
	}

	void sealed_validation_failure_is_not_authoritative()
	{
		auto observation = std::make_shared<spool_observation>();
		projection_port port;
		port.reject_sealed = true;
		auto prepared = materialization_report2_2_builder::prepare(
			std::make_unique<memory_spool>(observation), prepublication(), port, {1024U, 256U});
		require(prepared.has_value(), "sealed-validation fault fixture was rejected early");
		auto rejected = std::move(*prepared).finalize(terminal());
		require(!rejected && observation->seal_calls == 1U && port.validation_calls == 1U,
				"schema-invalid sealed bytes became an authoritative report");
	}

	void report_receipt_is_deterministic()
	{
		auto first_observation = std::make_shared<spool_observation>();
		auto second_observation = std::make_shared<spool_observation>();
		projection_port first_port;
		projection_port second_port;
		auto first = materialization_report2_2_builder::prepare(
			std::make_unique<memory_spool>(first_observation),
			prepublication(),
			first_port,
			{1024U, 256U});
		auto second = materialization_report2_2_builder::prepare(
			std::make_unique<memory_spool>(second_observation),
			prepublication(),
			second_port,
			{1024U, 256U});
		require(first.has_value() && second.has_value(), "determinism fixtures were rejected");
		auto first_sealed = std::move(*first).finalize(terminal());
		auto second_sealed = std::move(*second).finalize(terminal());
		require(first_sealed.has_value() && second_sealed.has_value() &&
					first_sealed->receipt() == second_sealed->receipt(),
				"equal report projections produced different receipts");
	}
} // namespace

int main()
{
	positive_two_phase_stream();
	expected_actual_mismatch_is_zero_write();
	phase_limits_fail_closed();
	terminal_must_bind_preparation();
	physical_reservation_precedes_encoding();
	cancellation_precedes_encoding();
	sealed_validation_failure_is_not_authoritative();
	report_receipt_is_deterministic();
	return 0;
}
