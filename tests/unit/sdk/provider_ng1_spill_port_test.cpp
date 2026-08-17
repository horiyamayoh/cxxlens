#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sdk/provider_ng1_spill_port_internal.hpp"

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
		auto output = semantic_digest("test.ng1.spill", value);
		require(output.has_value(), "test semantic digest construction failed");
		return *output;
	}

	[[nodiscard]] ng1_spill_binding binding()
	{
		return {"provider:test",
				"session:test",
				"task:test",
				"dependency:test",
				"atomic:test",
				"batch:test",
				7U};
	}

	[[nodiscard]] ng1_spill_record record(const ng1_spill_binding& value,
										  const std::uint64_t ordinal,
										  const std::uint64_t sequence,
										  const std::string_view payload_text)
	{
		ng1_spill_record output;
		output.record_ordinal = ordinal;
		output.task_id = value.task_id;
		output.dependency_group_id = value.dependency_group_id;
		output.atomic_output_group_id = value.atomic_output_group_id;
		output.batch_id = value.batch_id;
		output.stream_id = value.stream_id;
		output.sequence = sequence;
		for (const auto byte : payload_text)
			output.payload_bytes.push_back(
				static_cast<std::byte>(static_cast<unsigned char>(byte)));
		auto payload_digest = ng1_spill_payload_digest(output.payload_bytes);
		require(payload_digest.has_value(), "spill payload digest construction failed");
		output.payload_digest = *payload_digest;
		auto record_digest = ng1_spill_record_digest(output);
		require(record_digest.has_value(), "spill record digest construction failed");
		output.record_digest = *record_digest;
		return output;
	}

	[[nodiscard]] result<void> persist_frontier(std::optional<ng1_spill_resume_frontier>& current,
												const ng1_spill_resume_frontier& next)
	{
		if (auto valid = next.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (current &&
			(next.resume_generation <= current->resume_generation ||
			 next.receipt.fsync_sequence <= current->receipt.fsync_sequence))
			return unexpected(
				error{"provider.resume-token-stale", "resume_frontier", "not-increasing"});
		current = next;
		return {};
	}

	[[nodiscard]] ng1_spill_record record_with_payload_size(const ng1_spill_binding& value,
															const std::uint64_t ordinal,
															const std::uint64_t sequence,
															const std::size_t payload_size)
	{
		auto output = record(value, ordinal, sequence, "");
		output.payload_bytes.assign(payload_size, std::byte{0x5a});
		auto payload_digest = ng1_spill_payload_digest(output.payload_bytes);
		require(payload_digest.has_value(), "large spill payload digest construction failed");
		output.payload_digest = *payload_digest;
		auto record_digest = ng1_spill_record_digest(output);
		require(record_digest.has_value(), "large spill record digest construction failed");
		output.record_digest = *record_digest;
		return output;
	}

	class fake_spill_port final : public ng1_spill_storage_port
	{
	  public:
		bool fail_append{};
		std::size_t partial_append_bytes{};
		bool fail_fsync{};
		std::uint64_t fsync_value{1U};
		bool fail_cleanup{};
		std::vector<std::byte> bytes;
		std::optional<ng1_spill_resume_frontier> frontier;
		std::size_t append_calls{};
		std::size_t fsync_calls{};
		std::size_t cleanup_calls{};
		std::size_t* cleanup_calls_observer{};

		[[nodiscard]] result<void> append(const std::span<const std::byte> input) override
		{
			++append_calls;
			if (fail_append)
			{
				const auto count = std::min(partial_append_bytes, input.size());
				bytes.insert(
					bytes.end(), input.begin(), input.begin() + static_cast<std::ptrdiff_t>(count));
				return error{"provider.recovery-failed", "append", "injected"};
			}
			bytes.insert(bytes.end(), input.begin(), input.end());
			return {};
		}

		[[nodiscard]] result<std::uint64_t> fsync() override
		{
			++fsync_calls;
			if (fail_fsync)
				return error{"provider.recovery-failed", "fsync", "injected"};
			return fsync_value;
		}

		[[nodiscard]] result<std::vector<std::byte>> read_all() const override
		{
			return bytes;
		}

		[[nodiscard]] result<std::optional<ng1_spill_resume_frontier>>
		read_resume_frontier() const override
		{
			return frontier;
		}

		[[nodiscard]] result<void>
		persist_resume_frontier(const ng1_spill_resume_frontier& value) override
		{
			return persist_frontier(frontier, value);
		}

		[[nodiscard]] result<void> cleanup() override
		{
			++cleanup_calls;
			if (cleanup_calls_observer != nullptr)
				++*cleanup_calls_observer;
			if (fail_cleanup)
				return error{"provider.recovery-failed", "cleanup", "injected"};
			return {};
		}
	};

	void test_system_port_round_trip()
	{
		const auto spill_binding = binding();
		auto storage = make_system_ng1_spill_storage_port();
		if (!storage)
		{
			require(storage.error().code == "provider.recovery-failed",
					"unsupported spill platform did not fail closed");
			return;
		}
		auto session = ng1_spill_staging_session::create(spill_binding, std::move(*storage));
		require(session.has_value(), "system spill staging session creation failed");
		auto first = record(spill_binding, 0U, 0U, "first");
		require(session->append(first), "system spill append rejected a valid record");
		require(session->total_records() == 1U && session->total_bytes() > 0U,
				"system spill counters were not advanced");
		auto first_receipt = session->fsync(0U, 0U, digest("staged-0"), 1U);
		require(first_receipt.has_value(), "system spill fsync receipt construction failed");
		require(first_receipt->fsync_sequence == 1U && first_receipt->total_records == 1U,
				"system spill receipt lost fsync sequence or record count");

		auto recovered = session->recover();
		require(recovered.has_value(), "system spill recovery rejected a valid prefix");
		require(recovered->total_records() == session->total_records() &&
					recovered->total_bytes() == session->total_bytes(),
				"system spill recovery changed the exact prefix counters");

		auto second = record(spill_binding, 1U, 1U, "second");
		require(session->append(second), "system spill rejected a second contiguous record");
		auto second_receipt = session->fsync(1U, 1U, digest("staged-1"), 2U);
		require(second_receipt.has_value() && second_receipt->fsync_sequence == 2U,
				"system spill fsync sequence was not strictly increasing");
		require(session->cleanup(), "system spill cleanup failed");
		require(session->cleaned() && !session->cleanup(),
				"system spill cleanup was retryable after terminal disposal");
	}

	void test_restore_rehydrates_prefix_and_fsync_frontier()
	{
		const auto spill_binding = binding();
		auto first_storage = std::make_unique<fake_spill_port>();
		auto* first_raw = first_storage.get();
		auto first = ng1_spill_staging_session::create(spill_binding, std::move(first_storage));
		require(first.has_value(), "restore predecessor session creation failed");
		auto first_record = record(spill_binding, 0U, 0U, "first");
		require(first->append(first_record), "restore predecessor append failed");
		auto first_receipt = first->fsync(0U, 0U, digest("staged-0"), 1U);
		require(first_receipt.has_value(), "restore predecessor fsync failed");
		const auto durable_bytes = first_raw->bytes;
		const auto durable_frontier = first_raw->frontier;

		auto restart_storage = std::make_unique<fake_spill_port>();
		restart_storage->bytes = durable_bytes;
		restart_storage->frontier = durable_frontier;
		restart_storage->fsync_value = 2U;
		auto restarted =
			ng1_spill_staging_session::create(spill_binding, std::move(restart_storage));
		require(restarted.has_value(), "restore staging session creation failed");
		require(restarted->restore_from_fsync_receipt(*first_receipt, 1U),
				"restore rejected an exact durable prefix");
		require(restarted->total_records() == 1U &&
					restarted->total_bytes() == first->total_bytes(),
				"restore did not install the exact prefix counters");
		auto second_record = record(spill_binding, 1U, 1U, "second");
		require(restarted->append(second_record), "restored staging append failed");
		auto second_receipt = restarted->fsync(1U, 1U, digest("staged-1"), 2U);
		require(second_receipt.has_value() && second_receipt->fsync_sequence == 2U &&
					second_receipt->total_records == 2U,
				"restore did not retain the monotonic fsync frontier");
		require(first->cleanup(), "restore predecessor cleanup failed");
		require(restarted->cleanup(), "restored staging cleanup failed");

		auto corrupted_storage = std::make_unique<fake_spill_port>();
		corrupted_storage->bytes = durable_bytes;
		corrupted_storage->frontier = durable_frontier;
		auto corrupted =
			ng1_spill_staging_session::create(spill_binding, std::move(corrupted_storage));
		require(corrupted.has_value(), "restore corruption session creation failed");
		auto bad_receipt = *first_receipt;
		++bad_receipt.total_records;
		auto rejected = corrupted->restore_from_fsync_receipt(bad_receipt, 1U);
		require(!rejected && rejected.error().code == "provider.spill-corrupt" &&
					corrupted->poisoned(),
				"restore accepted a receipt for a different prefix");
		require(corrupted->cleanup(), "restore corruption cleanup failed");

		auto nonfresh_storage = std::make_unique<fake_spill_port>();
		auto nonfresh =
			ng1_spill_staging_session::create(spill_binding, std::move(nonfresh_storage));
		require(nonfresh.has_value(), "non-fresh restore session creation failed");
		require(nonfresh->append(first_record), "non-fresh restore setup append failed");
		auto nonfresh_result = nonfresh->restore_from_fsync_receipt(*first_receipt, 1U);
		require(!nonfresh_result && nonfresh_result.error().code == "provider.recovery-failed" &&
					nonfresh->poisoned(),
				"restore overlaid a non-fresh staging session");
		require(nonfresh->cleanup(), "non-fresh restore cleanup failed");
	}

	void test_append_failure_poison_and_atomicity()
	{
		const auto spill_binding = binding();
		auto storage = std::make_unique<fake_spill_port>();
		auto* raw_storage = storage.get();
		raw_storage->fail_append = true;
		auto session = ng1_spill_staging_session::create(spill_binding, std::move(storage));
		require(session.has_value(), "fake spill staging session creation failed");
		auto first = record(spill_binding, 0U, 0U, "first");
		auto append_result = session->append(first);
		require(!append_result && append_result.error().code == "provider.recovery-failed",
				"storage append failure did not remain a recovery failure");
		require(session->poisoned() && session->total_records() == 0U &&
					session->total_bytes() == 0U,
				"failed append mutated the validated prefix");
		auto retry_result = session->append(first);
		require(!retry_result && retry_result.error().code == "provider.recovery-failed" &&
					raw_storage->append_calls == 1U,
				"unknown append effect was retried");
		require(session->cleanup(), "poisoned spill cleanup failed");

		auto partial_storage = std::make_unique<fake_spill_port>();
		partial_storage->fail_append = true;
		partial_storage->partial_append_bytes = 3U;
		auto partial = ng1_spill_staging_session::create(spill_binding, std::move(partial_storage));
		require(partial.has_value(), "partial fake spill session creation failed");
		require(!partial->append(first), "partial append failure was accepted");
		auto recovered = partial->recover();
		require(!recovered && recovered.error().code == "provider.spill-corrupt",
				"torn last record did not fail closed during recovery");
		require(partial->cleanup(), "partial spill cleanup failed");

		auto accounting_storage = std::make_unique<fake_spill_port>();
		auto* accounting_raw = accounting_storage.get();
		auto accounting =
			ng1_spill_staging_session::create(spill_binding, std::move(accounting_storage));
		require(accounting.has_value(), "wire accounting spill session creation failed");
		require(accounting->append(first), "wire accounting append failed");
		require(accounting->total_bytes() == accounting_raw->bytes.size(),
				"wire-byte accounting omitted the framing prefix");
		require(accounting->cleanup(), "wire accounting cleanup failed");
	}

	void test_recovery_rejects_digest_corruption()
	{
		const auto spill_binding = binding();
		auto storage = std::make_unique<fake_spill_port>();
		auto* raw_storage = storage.get();
		auto session = ng1_spill_staging_session::create(spill_binding, std::move(storage));
		require(session.has_value(), "corruption fake spill session creation failed");
		auto first = record(spill_binding, 0U, 0U, "first");
		require(session->append(first), "corruption setup append failed");
		require(!raw_storage->bytes.empty(), "corruption setup did not create wire bytes");
		raw_storage->bytes.back() ^= std::byte{1U};
		auto recovered = session->recover();
		require(!recovered && recovered.error().code == "provider.spill-corrupt",
				"record digest corruption was accepted by recovery");
		require(session->cleanup(), "corruption spill cleanup failed");
	}

	void test_recovery_accepts_four_byte_cbor_lengths()
	{
		const auto spill_binding = binding();
		auto storage = std::make_unique<fake_spill_port>();
		auto session = ng1_spill_staging_session::create(spill_binding, std::move(storage));
		require(session.has_value(), "large-record spill session creation failed");
		const auto large = record_with_payload_size(spill_binding, 0U, 0U, 65'536U);
		require(session->append(large), "64 KiB spill record append failed");

		auto recovered = session->recover();
		require(recovered.has_value(),
				"valid four-byte CBOR length was rejected during spill recovery");
		require(recovered->total_bytes() == session->total_bytes() &&
					recovered->total_records() == 1U,
				"large spill recovery changed prefix accounting");
		require(session->cleanup(), "large-record spill cleanup failed");
	}

	void test_fsync_and_cleanup_fail_closed()
	{
		const auto spill_binding = binding();
		auto invalid_ack_storage = std::make_unique<fake_spill_port>();
		auto* invalid_ack_raw = invalid_ack_storage.get();
		auto invalid_ack =
			ng1_spill_staging_session::create(spill_binding, std::move(invalid_ack_storage));
		require(invalid_ack.has_value(), "invalid ack spill session creation failed");
		auto invalid_ack_result = invalid_ack->fsync(1U, 0U, digest("staged"), 1U);
		require(!invalid_ack_result &&
					invalid_ack_result.error().code == "provider.spill-corrupt" &&
					invalid_ack_raw->fsync_calls == 0U,
				"ahead-of-observed ACK reached the storage fsync port");
		require(invalid_ack->cleanup(), "invalid ack spill cleanup failed");

		auto invalid_sequence_storage = std::make_unique<fake_spill_port>();
		invalid_sequence_storage->fsync_value = 0U;
		auto invalid_sequence =
			ng1_spill_staging_session::create(spill_binding, std::move(invalid_sequence_storage));
		require(invalid_sequence.has_value(), "invalid sequence spill session creation failed");
		require(invalid_sequence->append(record(spill_binding, 0U, 0U, "sequence")),
				"invalid sequence setup append failed");
		auto invalid_sequence_result = invalid_sequence->fsync(0U, 0U, digest("staged"), 1U);
		require(!invalid_sequence_result &&
					invalid_sequence_result.error().code == "provider.recovery-failed" &&
					invalid_sequence->poisoned(),
				"invalid host fsync sequence did not poison the session");
		require(invalid_sequence->cleanup(), "invalid sequence spill cleanup failed");

		auto cleanup_storage = std::make_unique<fake_spill_port>();
		std::size_t cleanup_calls_observer{};
		cleanup_storage->cleanup_calls_observer = &cleanup_calls_observer;
		cleanup_storage->fail_cleanup = true;
		auto cleanup_session =
			ng1_spill_staging_session::create(spill_binding, std::move(cleanup_storage));
		require(cleanup_session.has_value(), "cleanup failure spill session creation failed");
		auto cleanup_result = cleanup_session->cleanup();
		require(!cleanup_result && cleanup_result.error().code == "provider.recovery-failed",
				"cleanup failure was not stable recovery-failed");
		auto cleanup_retry = cleanup_session->cleanup();
		require(!cleanup_retry && cleanup_retry.error().code == "provider.recovery-failed" &&
					cleanup_calls_observer == 1U,
				"cleanup unknown effect was retried");
	}
} // namespace

int main()
{
	test_system_port_round_trip();
	test_restore_rehydrates_prefix_and_fsync_frontier();
	test_append_failure_poison_and_atomicity();
	test_recovery_rejects_digest_corruption();
	test_recovery_accepts_four_byte_cbor_lengths();
	test_fsync_and_cleanup_fail_closed();
	return 0;
}
