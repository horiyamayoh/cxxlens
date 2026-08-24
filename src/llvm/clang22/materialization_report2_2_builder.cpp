#include "materialization_report2_2_builder.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <ranges>
#include <string>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error failure(std::string field, std::string detail = {})
		{
			return {"materialization.report-v2_2-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error limit_failure(std::string field, std::string detail = {})
		{
			return {"materialization.report-v2_2-limit", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool valid_text(const std::string_view value) noexcept
		{
			return !value.empty() && !value.contains('\0') && sdk::validate_utf8_text(value);
		}

		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		[[nodiscard]] sdk::result<void> strong(const std::string_view value,
											   const std::string_view field)
		{
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return sdk::unexpected(failure(std::string{field}, "strong-id"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate(const materialization_report2_2_prepublication_projection& value)
		{
			if (value.task_count == 0U || value.task_count > 4096U || value.closure_count == 0U ||
				value.closure_count > value.task_count)
				return sdk::unexpected(failure("prepublication.census", "count"));
			const std::array<std::pair<std::string_view, std::string_view>, 14U> ids{{
				{"materialization-request-id", value.materialization_request_id},
				{"request-digest", value.request_digest},
				{"semantic-request-digest", value.semantic_request_digest},
				{"request-authority-digest", value.request_authority_digest},
				{"closure-receipt-set-digest", value.closure_receipt_set_digest},
				{"worker-result-set-digest", value.worker_result_set_digest},
				{"base-row-result-set-digest", value.base_row_result_set_digest},
				{"task-receipt-digest", value.task_receipt_digest},
				{"store-source-digest", value.store_source_digest},
				{"store-preparation-digest", value.store_preparation_digest},
				{"expected-projection-digest", value.expected_projection_digest},
				{"actual-projection-digest", value.actual_projection_digest},
				{"journal-digest", value.journal_digest},
				{"schema", materialization_report2_2_schema},
			}};
			for (const auto& [field, id] : ids)
				if (auto valid = strong(id, field); !valid)
					return valid;
			if (value.expected_projection_digest != value.actual_projection_digest)
				return sdk::unexpected(
					failure("prepublication.store-projection", "expected-actual-mismatch"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate(const materialization_report2_2_terminal_projection& value)
		{
			if (!is_valid(value.terminal))
				return sdk::unexpected(failure("terminal.kind", "enum"));
			if (auto valid = strong(value.store_preparation_digest, "terminal.preparation-digest");
				!valid)
				return valid;
			if (auto valid = strong(value.store_result_digest, "terminal.result-digest"); !valid)
				return valid;

			const bool attempted =
				value.terminal != materialization_report2_2_store_terminal::not_attempted;
			if (attempted != (value.publish_call_count == 1U))
				return sdk::unexpected(failure("terminal.publish-call-count", "terminal-matrix"));
			const bool committed =
				value.terminal == materialization_report2_2_store_terminal::committed_unverified ||
				value.terminal == materialization_report2_2_store_terminal::committed_verified;
			if (committed != value.publication_id.has_value())
				return sdk::unexpected(failure("terminal.publication-id", "terminal-matrix"));
			if (value.publication_id)
			{
				if (!valid_text(*value.publication_id))
					return sdk::unexpected(failure("terminal.publication-id", "text"));
				if (auto valid = strong(*value.publication_id, "terminal.publication-id"); !valid)
					return valid;
			}
			return {};
		}

		[[nodiscard]] sdk::canonical_value text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::result<std::string>
		canonical_digest(const std::string_view domain, const sdk::canonical_value& projection)
		{
			auto encoded = sdk::canonical_binary(projection);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			std::string bytes;
			try
			{
				bytes.reserve(encoded->size());
				for (const auto byte : *encoded)
					bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(failure("projection", "allocation"));
			}
			return sdk::semantic_digest(domain, bytes);
		}
	} // namespace

	sdk::result<std::string> materialization_report2_2_prepublication_digest(
		const materialization_report2_2_prepublication_projection& projection)
	{
		const auto& value = projection;
		if (auto valid = validate(value); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return canonical_digest(
			"cxxlens.clang22.materialization-report-v2_2-prepublication.v1",
			sdk::canonical_value::from_tuple({
				text(materialization_report2_2_schema),
				text(materialization_report2_2_version),
				text(value.materialization_request_id),
				text(value.request_digest),
				text(value.semantic_request_digest),
				text(value.request_authority_digest),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.task_count)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.closure_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.unique_blob_bytes)),
				text(value.closure_receipt_set_digest),
				text(value.worker_result_set_digest),
				text(value.base_row_result_set_digest),
				text(value.task_receipt_digest),
				text(value.store_source_digest),
				text(value.store_preparation_digest),
				text(value.expected_projection_digest),
				text(value.actual_projection_digest),
				text(value.journal_digest),
			}));
	}

	sdk::result<std::string> materialization_report2_2_terminal_digest(
		const materialization_report2_2_terminal_projection& projection)
	{
		const auto& value = projection;
		if (auto valid = validate(value); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return canonical_digest(
			"cxxlens.clang22.materialization-report-v2_2-terminal.v1",
			sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.terminal)),
				text(value.store_preparation_digest),
				text(value.store_result_digest),
				value.publication_id ? text(*value.publication_id) : sdk::canonical_value::null(),
				sdk::canonical_value::from_integer(value.publish_call_count),
			}));
	}

	struct materialization_report2_2_builder::state
	{
		enum class phase : std::uint8_t
		{
			prepublication,
			terminal,
			finalized,
		};

		std::unique_ptr<materialization_report2_2_reserved_spool> storage;
		materialization_report2_2_prepublication_projection prepublication;
		materialization_report2_2_projection_port* port{};
		materialization_report2_2_limits limits;
		phase current{phase::prepublication};
		std::uint64_t prepublication_bytes{};
		std::uint64_t terminal_bytes{};
		bool terminal_reserved{};
		bool poisoned{};
	};

	namespace
	{
		class bounded_chunk_sink final : public materialization_report2_2_chunk_sink
		{
		  public:
			explicit bounded_chunk_sink(materialization_report2_2_builder::state& state)
				: state_{&state}
			{
			}

			sdk::result<void> append(const std::span<const std::byte> bytes) override
			{
				if (state_ == nullptr || state_->poisoned || !state_->storage ||
					state_->current == materialization_report2_2_builder::state::phase::finalized ||
					bytes.empty())
					return sdk::unexpected(failure("chunk", "state-or-empty"));
				if (bytes.size() > std::numeric_limits<std::uint64_t>::max())
					return sdk::unexpected(limit_failure("chunk", "size-overflow"));
				const auto count = static_cast<std::uint64_t>(bytes.size());
				std::uint64_t* phase_bytes = &state_->prepublication_bytes;
				std::uint64_t phase_limit =
					state_->limits.maximum_report_bytes - state_->limits.maximum_terminal_bytes;
				if (state_->current == materialization_report2_2_builder::state::phase::terminal)
				{
					phase_bytes = &state_->terminal_bytes;
					phase_limit = state_->limits.maximum_terminal_bytes;
				}
				if (*phase_bytes > phase_limit || count > phase_limit - *phase_bytes)
					return sdk::unexpected(limit_failure("chunk", "phase-maximum"));
				const auto total = state_->prepublication_bytes + state_->terminal_bytes;
				if (total > state_->limits.maximum_report_bytes ||
					count > state_->limits.maximum_report_bytes - total)
					return sdk::unexpected(limit_failure("report", "maximum"));
				if (auto appended = state_->storage->append(bytes); !appended)
				{
					state_->poisoned = true;
					return sdk::unexpected(failure("spool", "append"));
				}
				*phase_bytes += count;
				return {};
			}

		  private:
			materialization_report2_2_builder::state* state_;
		};
	} // namespace

	sealed_materialization_report2_2::sealed_materialization_report2_2(
		std::unique_ptr<materialization_replayable_spool> bytes,
		materialization_report2_2_receipt receipt)
		: bytes_{std::move(bytes)}, receipt_{std::move(receipt)}
	{
	}

	const materialization_report2_2_receipt&
	sealed_materialization_report2_2::receipt() const noexcept
	{
		return receipt_;
	}

	materialization_replayable_spool& sealed_materialization_report2_2::bytes() noexcept
	{
		return *bytes_;
	}

	std::unique_ptr<materialization_replayable_spool>
	sealed_materialization_report2_2::take_bytes() && noexcept
	{
		return std::move(bytes_);
	}

	materialization_report2_2_builder::materialization_report2_2_builder(
		std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}

	materialization_report2_2_builder::materialization_report2_2_builder(
		materialization_report2_2_builder&&) noexcept = default;
	materialization_report2_2_builder& materialization_report2_2_builder::operator=(
		materialization_report2_2_builder&&) noexcept = default;
	materialization_report2_2_builder::~materialization_report2_2_builder() = default;

	sdk::result<materialization_report2_2_builder> materialization_report2_2_builder::prepare(
		std::unique_ptr<materialization_report2_2_reserved_spool> storage,
		materialization_report2_2_prepublication_projection projection,
		materialization_report2_2_projection_port& port,
		const materialization_report2_2_limits limits,
		const std::stop_token& cancellation)
	{
		if (!storage || storage->sealed() || limits.maximum_report_bytes == 0U ||
			limits.maximum_terminal_bytes == 0U ||
			limits.maximum_terminal_bytes >= limits.maximum_report_bytes)
			return sdk::unexpected(failure("limits", "invalid"));
		if (auto valid = validate(projection); !valid)
			return sdk::unexpected(std::move(valid.error()));

		try
		{
			if (cancellation.stop_requested())
				return sdk::unexpected(failure("prepublication", "cancelled-before-encoding"));
			auto state_value = std::make_unique<state>();
			state_value->storage = std::move(storage);
			state_value->prepublication = std::move(projection);
			state_value->port = &port;
			state_value->limits = limits;
			bounded_chunk_sink sink{*state_value};
			if (auto written =
					port.write_prepublication(state_value->prepublication, sink, cancellation);
				!written)
				return sdk::unexpected(std::move(written.error()));
			if (cancellation.stop_requested())
				return sdk::unexpected(failure("prepublication", "cancelled-after-encoding"));
			if (state_value->prepublication_bytes == 0U || state_value->poisoned)
				return sdk::unexpected(failure("prepublication", "empty-or-poisoned"));
			if (auto reserved =
					state_value->storage->reserve_terminal_bytes(limits.maximum_terminal_bytes);
				!reserved ||
				state_value->storage->reserved_terminal_bytes() < limits.maximum_terminal_bytes)
				return sdk::unexpected(failure("prepublication", "terminal-reservation"));
			state_value->terminal_reserved = true;
			return materialization_report2_2_builder{std::move(state_value)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("prepublication", "allocation"));
		}
		catch (...)
		{
			return sdk::unexpected(failure("prepublication", "encoder-exception"));
		}
	}

	bool materialization_report2_2_builder::terminal_space_reserved() const noexcept
	{
		return state_ && state_->terminal_reserved && !state_->poisoned &&
			state_->current == state::phase::prepublication;
	}

	std::uint64_t materialization_report2_2_builder::prepublication_bytes() const noexcept
	{
		return state_ ? state_->prepublication_bytes : 0U;
	}

	sdk::result<sealed_materialization_report2_2> materialization_report2_2_builder::finalize(
		const materialization_report2_2_terminal_projection& terminal) &&
	{
		if (!state_ || state_->poisoned || !state_->storage || !state_->port ||
			!state_->terminal_reserved || state_->current != state::phase::prepublication)
			return sdk::unexpected(failure("terminal", "builder-state"));
		if (auto valid = validate(terminal); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (terminal.store_preparation_digest != state_->prepublication.store_preparation_digest)
			return sdk::unexpected(
				failure("terminal.preparation-digest", "prepublication-mismatch"));

		auto prepublication_digest =
			materialization_report2_2_prepublication_digest(state_->prepublication);
		auto terminal_digest = materialization_report2_2_terminal_digest(terminal);
		if (!prepublication_digest || !terminal_digest)
			return sdk::unexpected(!prepublication_digest ? std::move(prepublication_digest.error())
														  : std::move(terminal_digest.error()));

		try
		{
			state_->current = state::phase::terminal;
			bounded_chunk_sink sink{*state_};
			if (auto written = state_->port->write_terminal(terminal, sink); !written)
				return sdk::unexpected(std::move(written.error()));
			if (state_->terminal_bytes == 0U || state_->poisoned)
				return sdk::unexpected(failure("terminal", "empty-or-poisoned"));
			if (auto sealed = state_->storage->seal(); !sealed)
			{
				state_->poisoned = true;
				return sdk::unexpected(failure("spool", "seal"));
			}
			auto content = digest_materialization_spool(*state_->storage);
			if (!content)
			{
				state_->poisoned = true;
				return sdk::unexpected(failure("spool", "digest"));
			}
			auto semantic = sdk::semantic_digest("cxxlens.clang22-materialization-report.v2_2",
												 *content + "\n" + *prepublication_digest + "\n" +
													 *terminal_digest);
			if (!semantic)
				return sdk::unexpected(std::move(semantic.error()));
			materialization_report2_2_receipt receipt{
				std::string{materialization_report2_2_schema},
				std::string{materialization_report2_2_version},
				state_->prepublication_bytes + state_->terminal_bytes,
				std::move(*content),
				std::move(*semantic),
				std::move(*prepublication_digest),
				std::move(*terminal_digest),
				terminal.terminal,
			};
			if (auto valid = state_->port->validate_sealed(
					state_->prepublication, terminal, *state_->storage, receipt);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto rewound = state_->storage->rewind(); !rewound)
				return sdk::unexpected(failure("spool", "rewind"));
			state_->current = state::phase::finalized;
			auto storage = std::move(state_->storage);
			state_.reset();
			return sealed_materialization_report2_2{std::move(storage), std::move(receipt)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("terminal", "allocation"));
		}
		catch (...)
		{
			return sdk::unexpected(failure("terminal", "encoder-exception"));
		}
	}
} // namespace cxxlens::detail::clang22::materialization
