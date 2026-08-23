#include "materialization_bounded_report.hpp"

#include <limits>
#include <new>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool checked_add(const std::uint64_t left,
									   const std::uint64_t right,
									   const std::uint64_t limit,
									   std::uint64_t& output) noexcept
		{
			if (left > limit || right > limit - left)
				return false;
			output = left + right;
			return true;
		}
	} // namespace

	struct materialization_bounded_report_writer::state
	{
		state(std::unique_ptr<materialization_private_spool> storage_value,
			  materialization_bounded_report_limits limits_value) noexcept
			: storage{std::move(storage_value)}, limits{limits_value}
		{
		}

		std::unique_ptr<materialization_private_spool> storage;
		materialization_bounded_report_limits limits;
		std::uint64_t bytes{};
		std::optional<materialization_bounded_report_terminal> terminal;
		std::string content_digest;
		std::string semantic_digest;
		bool reserved{};
		bool finalized{};
		bool poisoned{};
	};

	materialization_bounded_report_writer::materialization_bounded_report_writer(
		std::unique_ptr<state> state_value)
		: state_{std::move(state_value)}
	{
	}

	materialization_bounded_report_writer::materialization_bounded_report_writer(
		materialization_bounded_report_writer&&) noexcept = default;
	materialization_bounded_report_writer& materialization_bounded_report_writer::operator=(
		materialization_bounded_report_writer&&) noexcept = default;
	materialization_bounded_report_writer::~materialization_bounded_report_writer() = default;

	sdk::result<materialization_bounded_report_writer>
	materialization_bounded_report_writer::create(
		std::unique_ptr<materialization_private_spool> storage,
		const materialization_bounded_report_limits limits)
	{
		if (!storage || limits.max_report_bytes == 0U || limits.max_tail_bytes == 0U ||
			limits.max_tail_bytes > limits.max_report_bytes)
			return sdk::unexpected(failure("materialization.report-invalid", "limits", "invalid"));
		try
		{
			return materialization_bounded_report_writer{
				std::make_unique<state>(std::move(storage), limits)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("materialization.report-invalid", "limits", "allocation"));
		}
	}

	sdk::result<void> materialization_bounded_report_writer::reserve()
	{
		if (!state_ || state_->poisoned || !state_->storage || state_->reserved ||
			state_->finalized)
			return sdk::unexpected(
				failure("materialization.report-invalid", "report-tail", "reserve-state"));
		state_->reserved = true;
		return {};
	}

	sdk::result<void>
	materialization_bounded_report_writer::append(const std::span<const std::byte> bytes)
	{
		if (!state_ || state_->poisoned || !state_->storage || !state_->reserved ||
			state_->finalized)
			return sdk::unexpected(
				failure("materialization.report-invalid", "report-tail", "append-state"));
		if (bytes.size() > std::numeric_limits<std::uint64_t>::max())
			return sdk::unexpected(
				failure("materialization.report-limit", "report-tail", "size-overflow"));
		std::uint64_t next{};
		if (!checked_add(state_->bytes,
						 static_cast<std::uint64_t>(bytes.size()),
						 state_->limits.max_tail_bytes,
						 next))
			return sdk::unexpected(
				failure("materialization.report-limit", "report-tail", "maximum"));
		if (!checked_add(next, 0U, state_->limits.max_report_bytes, next))
			return sdk::unexpected(failure("materialization.report-limit", "report", "maximum"));
		if (auto appended = state_->storage->append(bytes); !appended)
		{
			state_->poisoned = true;
			return sdk::unexpected(
				failure("materialization.report-spool", "report-tail", "append"));
		}
		state_->bytes = next;
		return {};
	}

	sdk::result<void> materialization_bounded_report_writer::finalize(
		const materialization_bounded_report_terminal terminal)
	{
		if (!state_ || state_->poisoned || !state_->storage || !state_->reserved ||
			state_->finalized || !is_valid(terminal))
			return sdk::unexpected(
				failure("materialization.report-invalid", "report-tail", "finalize-state"));
		if (auto sealed = state_->storage->seal(); !sealed)
		{
			state_->poisoned = true;
			return sdk::unexpected(failure("materialization.report-spool", "report-tail", "seal"));
		}
		auto* replayable = dynamic_cast<materialization_replayable_spool*>(state_->storage.get());
		if (replayable == nullptr)
		{
			state_->poisoned = true;
			return sdk::unexpected(
				failure("materialization.report-spool", "report-tail", "not-replayable"));
		}
		auto content = digest_materialization_spool(*replayable);
		if (!content)
		{
			state_->poisoned = true;
			return sdk::unexpected(
				failure("materialization.report-hash", "report-tail", "content-digest"));
		}
		auto semantic =
			sdk::semantic_digest("cxxlens.clang22-materialization-report-tail.v1", *content);
		if (!semantic)
		{
			state_->poisoned = true;
			return sdk::unexpected(std::move(semantic.error()));
		}
		state_->content_digest = std::move(*content);
		state_->semantic_digest = std::move(*semantic);
		state_->terminal = terminal;
		state_->finalized = true;
		return {};
	}

	bool materialization_bounded_report_writer::reserved() const noexcept
	{
		return state_ && state_->reserved;
	}
	bool materialization_bounded_report_writer::finalized() const noexcept
	{
		return state_ && state_->finalized;
	}
	std::uint64_t materialization_bounded_report_writer::bytes_written() const noexcept
	{
		return state_ ? state_->bytes : 0U;
	}
	std::optional<materialization_bounded_report_terminal>
	materialization_bounded_report_writer::terminal() const noexcept
	{
		return state_ ? state_->terminal : std::nullopt;
	}

	sdk::result<std::string> materialization_bounded_report_writer::content_digest() const
	{
		if (!state_ || !state_->finalized)
			return sdk::unexpected(
				failure("materialization.report-invalid", "report-tail", "digest-before-finalize"));
		return state_->content_digest;
	}

	sdk::result<std::string> materialization_bounded_report_writer::semantic_digest() const
	{
		if (!state_ || !state_->finalized)
			return sdk::unexpected(
				failure("materialization.report-invalid", "report-tail", "digest-before-finalize"));
		return state_->semantic_digest;
	}
} // namespace cxxlens::detail::clang22::materialization
