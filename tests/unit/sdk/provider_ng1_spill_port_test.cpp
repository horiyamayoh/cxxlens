#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sdk/provider_ng1_spill_port_internal.hpp"

#if defined(__linux__) && defined(__GLIBC__)
#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

#if defined(__linux__) && defined(__GLIBC__)
	using spill_test_path = std::filesystem::path;

	[[nodiscard]] std::vector<spill_test_path> existing_system_spill_directories()
	{
		return {};
	}

	[[nodiscard]] spill_test_path
	new_system_spill_directory(const std::vector<spill_test_path>& before)
	{
		(void)before;
		spill_test_path output;
		std::error_code filesystem_error;
		std::filesystem::directory_iterator iterator{"/proc/self/fd", filesystem_error};
		const std::filesystem::directory_iterator end;
		while (!filesystem_error && iterator != end)
		{
			std::error_code link_error;
			const auto target = std::filesystem::read_symlink(iterator->path(), link_error);
			if (!link_error && target.filename() == "spill.data" &&
				target.parent_path().filename().string().starts_with("cxxlens-ng1-spill-"))
			{
				if (output.empty())
					output = target.parent_path();
				else
					require(output == target.parent_path(),
							"more than one private spill directory was open in this process");
			}
			iterator.increment(filesystem_error);
		}
		require(!filesystem_error && !output.empty(),
				"system spill directory was not discoverable through the process descriptor table");
		return output;
	}

	[[nodiscard]] bool child_rejects_before_deadline(auto&& operation)
	{
		const auto child = ::fork();
		if (child < 0)
			return false;
		if (child == 0)
		{
			const auto result = operation();
			::_exit(!result ? 0 : 1);
		}

		int status{};
		bool timed_out{};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
		for (;;)
		{
			const auto waited = ::waitpid(child, &status, WNOHANG);
			if (waited == child)
				break;
			if (waited < 0 && errno != EINTR)
				return false;
			if (std::chrono::steady_clock::now() >= deadline)
			{
				timed_out = true;
				(void)::kill(child, SIGKILL);
				while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
				{
				}
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds{5});
		}
		return !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}

	void remove_spill_path(const spill_test_path& path)
	{
		std::error_code filesystem_error;
		std::filesystem::remove(path, filesystem_error);
		require(!filesystem_error, "could not remove manipulated spill path");
	}

	void make_fifo_path(const spill_test_path& path)
	{
		require(::mkfifo(path.c_str(), static_cast<mode_t>(0600)) == 0,
				"could not create spill FIFO fixture");
	}

	void write_path_bytes(const spill_test_path& path, const std::string_view value)
	{
		const auto descriptor = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
		require(descriptor >= 0, "could not open manipulated spill metadata");
		const auto* begin = value.data();
		std::size_t remaining = value.size();
		while (remaining > 0U)
		{
			const auto written = ::write(descriptor, begin, remaining);
			if (written > 0)
			{
				begin += written;
				remaining -= static_cast<std::size_t>(written);
				continue;
			}
			require(written < 0 && errno == EINTR, "could not write manipulated spill metadata");
		}
		require(::fsync(descriptor) == 0, "could not fsync manipulated spill metadata");
		require(::close(descriptor) == 0, "could not close manipulated spill metadata");
	}

	void create_empty_regular_path(const spill_test_path& path)
	{
		const auto descriptor = ::open(path.c_str(),
									   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
									   static_cast<mode_t>(0600));
		require(descriptor >= 0, "could not create replacement spill file");
		require(::close(descriptor) == 0, "could not close replacement spill file");
	}
#endif

	class fake_spill_port final : public ng1_spill_storage_port
	{
	  public:
		bool fail_append{};
		std::size_t partial_append_bytes{};
		bool fail_fsync{};
		std::uint64_t fsync_value{1U};
		bool fail_cleanup{};
		bool cleanup_custody{true};
		std::vector<std::byte> bytes;
		std::optional<ng1_spill_resume_frontier> frontier;
		std::size_t append_calls{};
		std::size_t fsync_calls{};
		std::size_t cleanup_calls{};
		std::size_t transfer_calls{};
		mutable std::size_t read_calls{};
		mutable std::size_t reopen_calls{};
		std::size_t* cleanup_calls_observer{};
		std::size_t* transfer_calls_observer{};

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
			++read_calls;
			return bytes;
		}

		[[nodiscard]] result<std::unique_ptr<ng1_spill_storage_port>> reopen() const override
		{
			++reopen_calls;
			auto output = std::make_unique<fake_spill_port>();
			output->bytes = bytes;
			output->frontier = frontier;
			output->fsync_value = fsync_value;
			output->cleanup_custody = false;
			return std::unique_ptr<ng1_spill_storage_port>{std::move(output)};
		}

		[[nodiscard]] result<void>
		transfer_cleanup_custody_to(ng1_spill_storage_port& replacement) override
		{
			++transfer_calls;
			if (transfer_calls_observer != nullptr)
				++*transfer_calls_observer;
			auto* target = dynamic_cast<fake_spill_port*>(&replacement);
			if (target == nullptr || target == this || !cleanup_custody || target->cleanup_custody)
				return error{"provider.recovery-failed", "cleanup", "injected-custody-mismatch"};
			cleanup_custody = false;
			target->cleanup_custody = true;
			return {};
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

	void test_system_port_reopen_recovers_highest_frontier()
	{
		const auto spill_binding = binding();
		auto storage = make_system_ng1_spill_storage_port();
		if (!storage)
		{
			require(storage.error().code == "provider.recovery-failed",
					"unsupported spill platform did not fail closed during reopen test");
			return;
		}
		auto* original_storage = storage->get();
		auto session = ng1_spill_staging_session::create(spill_binding, std::move(*storage));
		require(session.has_value(), "system reopen predecessor session creation failed");
		require(session->append(record(spill_binding, 0U, 0U, "first")),
				"system reopen first append failed");
		auto first_receipt = session->fsync(0U, 0U, digest("reopen-0"), 1U);
		require(first_receipt.has_value(), "system reopen first fsync failed");
		require(session->append(record(spill_binding, 1U, 1U, "second")),
				"system reopen second append failed");
		auto second_receipt = session->fsync(1U, 1U, digest("reopen-1"), 2U);
		require(second_receipt.has_value() && second_receipt->fsync_sequence == 2U,
				"system reopen second fsync did not advance");

		auto reopened_storage = original_storage->reopen();
		require(reopened_storage.has_value(), "system spill durable object did not reopen");
		auto persisted = (*reopened_storage)->read_resume_frontier();
		require(persisted.has_value() && persisted->has_value() &&
					persisted->value().resume_generation == 2U &&
					persisted->value().receipt.fsync_sequence == 2U,
				"reopen did not retain the highest durable frontier");
		auto restarted =
			ng1_spill_staging_session::create(spill_binding, std::move(*reopened_storage));
		require(restarted.has_value(), "system reopen restart session creation failed");
		require(restarted->restore_from_fsync_receipt(*second_receipt, 2U),
				"system reopen rejected the latest durable receipt");
		require(session->handoff_cleanup_custody_to(*restarted),
				"system reopen staging handoff did not retire the old cleanup owner");
		require(restarted->total_records() == 2U, "system reopen changed the durable record count");
		require(restarted->append(record(spill_binding, 2U, 2U, "third")),
				"system reopen append after recovery failed");
		auto third_receipt = restarted->fsync(2U, 2U, digest("reopen-2"), 3U);
		require(third_receipt.has_value() && third_receipt->fsync_sequence == 3U,
				"system reopen did not continue the durable fsync sequence");
		require(restarted->cleanup(), "system reopen restart cleanup failed");
		require(!session->cleanup(), "retired system reopen predecessor remained reusable");
		(void)first_receipt;
	}

	void test_system_port_reopen_discards_unpublished_commit()
	{
		const auto spill_binding = binding();
		auto storage = make_system_ng1_spill_storage_port();
		if (!storage)
		{
			require(storage.error().code == "provider.recovery-failed",
					"unsupported spill platform did not fail closed during crash-window test");
			return;
		}
		auto* original_storage = storage->get();
		auto session = ng1_spill_staging_session::create(spill_binding, std::move(*storage));
		require(session.has_value(), "crash-window predecessor session creation failed");
		require(session->append(record(spill_binding, 0U, 0U, "published")),
				"crash-window published append failed");
		auto published_receipt = session->fsync(0U, 0U, digest("published"), 1U);
		require(published_receipt.has_value(), "crash-window published fsync failed");
		require(session->append(record(spill_binding, 1U, 1U, "unpublished")),
				"crash-window unpublished append failed");
		auto unpublished_commit = original_storage->fsync();
		require(unpublished_commit.has_value() && *unpublished_commit == 2U,
				"crash-window setup did not create an unpublished commit generation");
		auto reopened_storage = original_storage->reopen();
		require(reopened_storage.has_value(), "crash-window durable object did not reopen");
		auto persisted = (*reopened_storage)->read_resume_frontier();
		require(persisted.has_value() && persisted->has_value() &&
					persisted->value().resume_generation == 1U &&
					persisted->value().receipt.fsync_sequence == 1U,
				"reopen did not retain the previous published frontier");
		auto restarted =
			ng1_spill_staging_session::create(spill_binding, std::move(*reopened_storage));
		require(restarted.has_value(), "crash-window restart session creation failed");
		require(restarted->restore_from_fsync_receipt(*published_receipt, 1U),
				"reopen rejected the previous published frontier after rollback");
		require(restarted->total_records() == 1U, "reopen retained an unpublished spill record");
		require(restarted->append(record(spill_binding, 1U, 1U, "unpublished")),
				"reopen could not restage the unpublished record");
		auto republished = restarted->fsync(1U, 1U, digest("republished"), 2U);
		require(republished.has_value() && republished->fsync_sequence == 2U,
				"reopen did not reset the next fsync sequence to the published frontier");
		require(restarted->cleanup(), "crash-window restart cleanup failed");
		require(session->cleanup(), "crash-window predecessor cleanup failed");

		auto empty_storage = make_system_ng1_spill_storage_port();
		if (!empty_storage)
		{
			require(empty_storage.error().code == "provider.recovery-failed",
					"unsupported spill platform changed during empty crash-window test");
			return;
		}
		auto* empty_raw = empty_storage->get();
		auto empty_commit = empty_raw->fsync();
		require(empty_commit.has_value() && *empty_commit == 1U,
				"empty crash-window setup did not create a commit marker");
		auto empty_reopened = empty_raw->reopen();
		require(empty_reopened.has_value(), "empty spill object did not reopen");
		auto empty_frontier = (*empty_reopened)->read_resume_frontier();
		require(empty_frontier.has_value() && !empty_frontier->has_value(),
				"unpublished commit without a frontier became resume authority");
		auto reset_sequence = (*empty_reopened)->fsync();
		require(reset_sequence.has_value() && *reset_sequence == 1U,
				"empty unpublished commit was not rolled back before reuse");
		require((*empty_reopened)->cleanup(), "empty crash-window reopened cleanup failed");
		require(empty_raw->cleanup(), "empty crash-window predecessor cleanup failed");
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

		auto mismatch_storage = std::make_unique<fake_spill_port>();
		auto mismatch_binding = spill_binding;
		mismatch_binding.task_id = "task:other";
		auto mismatch =
			ng1_spill_staging_session::create(mismatch_binding, std::move(mismatch_storage));
		require(mismatch.has_value(), "custody mismatch session creation failed");
		auto custody_source_storage = std::make_unique<fake_spill_port>();
		auto custody_source =
			ng1_spill_staging_session::create(spill_binding, std::move(custody_source_storage));
		require(custody_source.has_value(), "custody source session creation failed");
		auto custody_mismatch = custody_source->handoff_cleanup_custody_to(*mismatch);
		require(!custody_mismatch && custody_mismatch.error().code == "provider.spill-corrupt" &&
					!custody_source->cleaned() && !mismatch->cleaned(),
				"custody handoff mutated sessions before binding validation");
		require(custody_source->cleanup(), "custody source cleanup failed");
		require(mismatch->cleanup(), "custody mismatch cleanup failed");
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
		auto* partial_raw = partial_storage.get();
		partial_storage->fail_append = true;
		partial_storage->partial_append_bytes = 3U;
		auto partial = ng1_spill_staging_session::create(spill_binding, std::move(partial_storage));
		require(partial.has_value(), "partial fake spill session creation failed");
		require(!partial->append(first), "partial append failure was accepted");
		auto recovered = partial->recover();
		require(!recovered && recovered.error().code == "provider.recovery-failed" &&
					partial_raw->read_calls == 0U,
				"poisoned partial append was reread through the storage port");
		require(partial->cleanup(), "partial spill cleanup failed");

		auto torn_storage = std::make_unique<fake_spill_port>();
		torn_storage->bytes.assign(3U, std::byte{0x01});
		auto torn = ng1_spill_staging_session::create(spill_binding, std::move(torn_storage));
		require(torn.has_value(), "torn-prefix fake spill session creation failed");
		auto torn_recovered = torn->recover();
		require(!torn_recovered && torn_recovered.error().code == "provider.spill-corrupt",
				"torn last record did not fail closed during fresh recovery");
		require(torn->cleanup(), "torn-prefix spill cleanup failed");

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

	void test_custody_handoff_retire_failure_is_terminal_and_cleanup_remains_owned()
	{
		const auto spill_binding = binding();
		auto source_storage = std::make_unique<fake_spill_port>();
		auto* source_raw = source_storage.get();
		std::size_t source_cleanup_calls{};
		std::size_t source_transfer_calls{};
		source_raw->fail_cleanup = true;
		source_raw->cleanup_calls_observer = &source_cleanup_calls;
		source_raw->transfer_calls_observer = &source_transfer_calls;
		auto source = ng1_spill_staging_session::create(spill_binding, std::move(source_storage));
		require(source.has_value(), "retire-failure source session creation failed");
		require(source->append(record(spill_binding, 0U, 0U, "retire-failure")),
				"retire-failure source append failed");
		auto receipt = source->fsync(0U, 0U, digest("retire-failure"), 1U);
		require(receipt.has_value(), "retire-failure source fsync failed");

		auto replacement_storage = source_raw->reopen();
		require(replacement_storage.has_value(), "retire-failure replacement reopen failed");
		auto* replacement_raw = dynamic_cast<fake_spill_port*>(replacement_storage->get());
		require(replacement_raw != nullptr,
				"retire-failure replacement changed its storage implementation");
		std::size_t replacement_cleanup_calls{};
		replacement_raw->cleanup_calls_observer = &replacement_cleanup_calls;
		auto replacement =
			ng1_spill_staging_session::create(spill_binding, std::move(*replacement_storage));
		require(replacement.has_value(), "retire-failure replacement session creation failed");
		require(replacement->restore_from_fsync_receipt(*receipt, 1U),
				"retire-failure replacement did not restore the durable prefix");

		auto handed_off = source->handoff_cleanup_custody_to(*replacement);
		require(!handed_off && handed_off.error().code == "provider.recovery-failed" &&
					source->cleaned() && source->poisoned() && replacement_raw->cleanup_custody &&
					source_transfer_calls == 1U && source_cleanup_calls == 1U,
				"post-transfer source retirement failure did not fail closed");
		auto source_retry = source->cleanup();
		require(!source_retry,
				"post-transfer source retirement failure retried an unknown cleanup effect");
		require(replacement->cleanup() && replacement_cleanup_calls == 1U,
				"post-transfer retirement failure lost or duplicated replacement cleanup custody");
	}

	void test_system_port_rejects_special_files_before_blocking()
	{
#if defined(__linux__) && defined(__GLIBC__)
		for (const auto file_name : {"spill.data", "spill.commit", "spill.frontier"})
		{
			const auto before = existing_system_spill_directories();
			auto storage = make_system_ng1_spill_storage_port();
			if (!storage)
			{
				require(storage.error().code == "provider.recovery-failed",
						"unsupported spill platform changed during special-file test");
				return;
			}
			const auto directory = new_system_spill_directory(before);
			const auto path = directory / file_name;
			if (std::filesystem::exists(path))
				require(::unlink(path.c_str()) == 0, "could not remove regular spill fixture");
			make_fifo_path(path);
			const auto rejected = child_rejects_before_deadline(
				[&]()
				{
					return (*storage)->reopen();
				});
			remove_spill_path(path);
			const auto cleaned = (*storage)->cleanup();
			require(rejected, "special spill file was accepted or reopen blocked past deadline");
			require(cleaned, "special spill file cleanup did not complete");
		}
#endif
	}

	void test_system_port_rejects_data_inode_replacement()
	{
#if defined(__linux__) && defined(__GLIBC__)
		const auto before = existing_system_spill_directories();
		auto storage = make_system_ng1_spill_storage_port();
		if (!storage)
		{
			require(storage.error().code == "provider.recovery-failed",
					"unsupported spill platform changed during inode-replacement test");
			return;
		}
		const auto directory = new_system_spill_directory(before);
		const auto data_path = directory / "spill.data";
		const auto saved_data_path = directory / "spill.data.saved";
		require(::rename(data_path.c_str(), saved_data_path.c_str()) == 0,
				"could not preserve original spill data inode");
		create_empty_regular_path(data_path);
		auto reopened = (*storage)->reopen();
		const auto rejected = !reopened;
		remove_spill_path(data_path);
		require(::rename(saved_data_path.c_str(), data_path.c_str()) == 0,
				"could not restore original spill data inode");
		const auto cleaned = (*storage)->cleanup();
		require(rejected, "reopen accepted a replaced spill.data inode");
		require(cleaned, "inode-replacement spill cleanup did not complete");
#endif
	}

	void test_system_port_reads_persisted_frontier_from_disk()
	{
#if defined(__linux__) && defined(__GLIBC__)
		const auto delete_before = existing_system_spill_directories();
		auto delete_storage = make_system_ng1_spill_storage_port();
		if (!delete_storage)
		{
			require(delete_storage.error().code == "provider.recovery-failed",
					"unsupported spill platform changed during frontier-disk test");
			return;
		}
		auto* delete_raw = delete_storage->get();
		auto delete_session =
			ng1_spill_staging_session::create(binding(), std::move(*delete_storage));
		require(delete_session.has_value(), "frontier deletion session creation failed");
		require(delete_session->append(record(binding(), 0U, 0U, "frontier-delete")),
				"frontier deletion append failed");
		auto delete_receipt = delete_session->fsync(0U, 0U, digest("frontier-delete"), 1U);
		require(delete_receipt.has_value(), "frontier deletion fsync failed");
		const auto delete_directory = new_system_spill_directory(delete_before);
		const auto frontier_path = delete_directory / "spill.frontier";
		require(::unlink(frontier_path.c_str()) == 0, "could not delete persisted frontier");
		auto deleted_read = delete_raw->read_resume_frontier();
		auto deleted_validation = delete_session->validate_persisted_frontier(*delete_receipt, 1U);
		const auto deleted_cleanup = delete_session->cleanup();
		require(deleted_read.has_value() && !deleted_read->has_value() && !deleted_validation &&
					deleted_validation.error().code == "provider.resume-token-stale" &&
					delete_session->poisoned(),
				"deleted persisted frontier was served from cache or passed validation");
		require(deleted_cleanup, "frontier deletion cleanup did not complete");

		const auto corrupt_before = existing_system_spill_directories();
		auto corrupt_storage = make_system_ng1_spill_storage_port();
		require(corrupt_storage.has_value(), "frontier corruption storage creation failed");
		auto* corrupt_raw = corrupt_storage->get();
		auto corrupt_session =
			ng1_spill_staging_session::create(binding(), std::move(*corrupt_storage));
		require(corrupt_session.has_value(), "frontier corruption session creation failed");
		require(corrupt_session->append(record(binding(), 0U, 0U, "frontier-corrupt")),
				"frontier corruption append failed");
		auto corrupt_receipt = corrupt_session->fsync(0U, 0U, digest("frontier-corrupt"), 1U);
		require(corrupt_receipt.has_value(), "frontier corruption fsync failed");
		const auto corrupt_directory = new_system_spill_directory(corrupt_before);
		const auto corrupt_frontier_path = corrupt_directory / "spill.frontier";
		write_path_bytes(corrupt_frontier_path, "not-a-frontier");
		auto corrupted_read = corrupt_raw->read_resume_frontier();
		auto corrupted_validation =
			corrupt_session->validate_persisted_frontier(*corrupt_receipt, 1U);
		auto corrupted_reopen = corrupt_raw->reopen();
		const auto corrupted_cleanup = corrupt_session->cleanup();
		require(!corrupted_read && !corrupted_validation && corrupt_session->poisoned() &&
					!corrupted_reopen,
				"corrupted persisted frontier was served from cache or passed validation");
		require(corrupted_cleanup, "frontier corruption cleanup did not complete");
#else
		return;
#endif
	}

	void test_system_port_poisoning_blocks_reuse()
	{
#if defined(__linux__) && defined(__GLIBC__)
		const auto before = existing_system_spill_directories();
		auto storage = make_system_ng1_spill_storage_port();
		if (!storage)
		{
			require(storage.error().code == "provider.recovery-failed",
					"unsupported spill platform changed during poisoned-storage test");
			return;
		}
		const auto directory = new_system_spill_directory(before);
		const auto data_path = directory / "spill.data";
		require(::unlink(data_path.c_str()) == 0, "could not remove poisoned spill data");
		create_empty_regular_path(data_path);
		const std::array<std::byte, 1U> byte{std::byte{0x01}};
		auto append_result = (*storage)->append(byte);
		auto read_result = (*storage)->read_all();
		auto reopen_result = (*storage)->reopen();
		remove_spill_path(data_path);
		const auto cleaned = (*storage)->cleanup();
		require(!append_result && !read_result && !reopen_result,
				"poisoned spill storage remained readable or reopenable");
		require(cleaned, "poisoned-storage cleanup did not complete");
#endif
	}

	void test_oversized_record_is_rejected_before_storage_append()
	{
		const auto spill_binding = binding();
		auto storage = std::make_unique<fake_spill_port>();
		auto* raw_storage = storage.get();
		raw_storage->fail_append = true;
		auto session = ng1_spill_staging_session::create(spill_binding, std::move(storage));
		require(session.has_value(), "oversized-record spill session creation failed");
		auto oversized = record(spill_binding, 0U, 0U, "");
		// Keep the old digest deliberately: an exact wire-quota check must win before
		// payload/record hashing or codec allocation.
		oversized.payload_bytes.resize(static_cast<std::size_t>(ng1_spill_maximum_record_bytes),
									   std::byte{0x5a});
		auto append_result = session->append(oversized);
		require(!append_result && append_result.error().code == "provider.spill-corrupt" &&
					append_result.error().field == "record_bytes" &&
					append_result.error().detail == "record-quota" &&
					raw_storage->append_calls == 0U && !session->poisoned(),
				"oversized record reached storage or poisoned a pre-write rejection");
		require(session->cleanup(), "oversized-record spill cleanup failed");
	}

	void test_system_port_rejects_special_atomic_destination()
	{
#if defined(__linux__) && defined(__GLIBC__)
		const auto before = existing_system_spill_directories();
		auto storage = make_system_ng1_spill_storage_port();
		if (!storage)
		{
			require(storage.error().code == "provider.recovery-failed",
					"unsupported spill platform changed during atomic-destination test");
			return;
		}
		auto* raw_storage = storage->get();
		auto session = ng1_spill_staging_session::create(binding(), std::move(*storage));
		require(session.has_value(), "atomic-destination spill session creation failed");
		require(session->append(record(binding(), 0U, 0U, "atomic-destination")),
				"atomic-destination spill append failed");
		const auto directory = new_system_spill_directory(before);
		const auto commit_path = directory / "spill.commit";
		const auto saved_commit_path = directory / "spill.commit.saved";
		require(::rename(commit_path.c_str(), saved_commit_path.c_str()) == 0,
				"could not preserve the commit destination");
		make_fifo_path(commit_path);

		auto fsync_result = session->fsync(0U, 0U, digest("atomic-destination"), 1U);
		struct stat destination_metadata{};
		const auto fifo_preserved = ::lstat(commit_path.c_str(), &destination_metadata) == 0 &&
			S_ISFIFO(destination_metadata.st_mode);
		auto poisoned_read = raw_storage->read_all();
		auto poisoned_reopen = raw_storage->reopen();
		remove_spill_path(commit_path);
		require(::rename(saved_commit_path.c_str(), commit_path.c_str()) == 0,
				"could not restore the commit destination");
		const auto cleaned = session->cleanup();
		require(!fsync_result && fifo_preserved && !poisoned_read && !poisoned_reopen &&
					session->poisoned(),
				"atomic publication replaced a special destination or left the port reusable");
		require(cleaned, "atomic-destination spill cleanup did not complete");
#endif
	}

	void test_system_port_rejects_special_destination_during_custody_transfer()
	{
#if defined(__linux__) && defined(__GLIBC__)
		const auto before = existing_system_spill_directories();
		auto storage = make_system_ng1_spill_storage_port();
		if (!storage)
		{
			require(storage.error().code == "provider.recovery-failed",
					"unsupported spill platform changed during custody-special test");
			return;
		}
		const auto directory = new_system_spill_directory(before);
		const auto data_path = directory / "spill.data";
		auto replacement = (*storage)->reopen();
		require(replacement.has_value(), "custody-special replacement reopen failed");
		require(::unlink(data_path.c_str()) == 0, "could not remove custody-special data");
		make_fifo_path(data_path);
		auto transfer = (*storage)->transfer_cleanup_custody_to(**replacement);
		remove_spill_path(data_path);
		const auto source_cleanup = (*storage)->cleanup();
		const auto replacement_cleanup = (*replacement)->cleanup();
		require(!transfer, "custody transfer accepted a special-file destination");
		require(source_cleanup && replacement_cleanup, "custody-special cleanup did not complete");
#endif
	}
} // namespace

int main()
{
	test_system_port_round_trip();
	test_system_port_reopen_recovers_highest_frontier();
	test_system_port_reopen_discards_unpublished_commit();
	test_restore_rehydrates_prefix_and_fsync_frontier();
	test_append_failure_poison_and_atomicity();
	test_recovery_rejects_digest_corruption();
	test_recovery_accepts_four_byte_cbor_lengths();
	test_fsync_and_cleanup_fail_closed();
	test_custody_handoff_retire_failure_is_terminal_and_cleanup_remains_owned();
	test_system_port_rejects_special_files_before_blocking();
	test_system_port_rejects_data_inode_replacement();
	test_system_port_reads_persisted_frontier_from_disk();
	test_system_port_poisoning_blocks_reuse();
	test_oversized_record_is_rejected_before_storage_append();
	test_system_port_rejects_special_atomic_destination();
	test_system_port_rejects_special_destination_during_custody_transfer();
	return 0;
}
