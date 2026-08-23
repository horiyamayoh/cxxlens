#include "llvm/clang22/source_closure_transport.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using namespace cxxlens::detail::clang22;
	using cxxlens::sdk::result;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	template <class T>
	void require(const cxxlens::sdk::result<T>& value, const std::string_view message)
	{
		require(value.has_value(), message);
	}

	[[nodiscard]] std::string typed(const std::string_view prefix, const char digit)
	{
		return std::string{prefix} + std::string(64U, digit);
	}

	[[nodiscard]] source_closure_transfer_binding binding(const char digit = '1')
	{
		source_closure_transfer_binding output;
		output.session_id = typed("provider-session:sha256:", digit);
		output.task_v4_digest = typed("semantic-v2:sha256:", static_cast<char>(digit + 1));
		output.task_id = "task:" + output.task_v4_digest;
		output.closure_digest = typed("semantic-v2:sha256:", static_cast<char>(digit + 2));
		output.closure_id = "source-closure:" + output.closure_digest;
		output.manifest_digest = typed("semantic-v2:sha256:", static_cast<char>(digit + 3));
		output.first_sequence = 0U;
		return output;
	}

	class test_authority final : public source_closure_task_v4_authority
	{
	  public:
		explicit test_authority(const source_closure_transfer_binding& value)
			: task_id_value{value.task_id}, task_v4_digest_value{value.task_v4_digest}
		{
		}

		bool valid{true};
		mutable std::uint64_t calls{};
		std::string task_id_value;
		std::string task_v4_digest_value;

		[[nodiscard]] std::string_view task_id() const noexcept override
		{
			return task_id_value;
		}
		[[nodiscard]] std::string_view task_v4_digest() const noexcept override
		{
			return task_v4_digest_value;
		}

		[[nodiscard]] result<void> revalidate() const override
		{
			++calls;
			if (!valid)
				return cxxlens::sdk::unexpected(cxxlens::sdk::error{
					"source-closure.task-binding-mismatch", "outer-task", "fixture"});
			return {};
		}
	};

	class test_sink final : public source_closure_transfer_sink
	{
	  public:
		source_closure_manifest_summary summary;
		source_closure_ack_credentials credentials{
			typed("spool-receipt:semantic-v2:", 'a'), typed("cleanup-owner:semantic-v2:", 'b'), {}};
		std::string cleanup_receipt{typed("cleanup-receipt:semantic-v2:", 'c')};
		std::uint64_t manifest_begin_calls{};
		std::uint64_t manifest_append_calls{};
		std::uint64_t blob_begin_calls{};
		std::uint64_t blob_append_calls{};
		std::uint64_t closure_finish_calls{};
		std::uint64_t cleanup_calls{};
		bool fail_cleanup{false};

		[[nodiscard]] result<void>
		begin_manifest(const source_closure_manifest_descriptor&) override
		{
			++manifest_begin_calls;
			return {};
		}

		[[nodiscard]] result<void> append_manifest(std::span<const std::byte>) override
		{
			++manifest_append_calls;
			return {};
		}

		[[nodiscard]] result<source_closure_manifest_summary>
		finish_manifest(std::string_view) override
		{
			return summary;
		}

		[[nodiscard]] result<void> begin_blob(const source_closure_blob_descriptor&) override
		{
			++blob_begin_calls;
			return {};
		}

		[[nodiscard]] result<void> append_blob(std::span<const std::byte>) override
		{
			++blob_append_calls;
			return {};
		}

		[[nodiscard]] result<void> finish_blob(const source_closure_blob_receipt&) override
		{
			return {};
		}

		[[nodiscard]] result<source_closure_ack_credentials>
		finish_closure(std::string_view transfer_digest) override
		{
			++closure_finish_calls;
			credentials.transfer_digest = transfer_digest;
			return credentials;
		}

		[[nodiscard]] result<std::string> cleanup() override
		{
			++cleanup_calls;
			if (fail_cleanup)
				return cxxlens::sdk::unexpected(
					cxxlens::sdk::error{"source-closure.cleanup-failed", "cleanup", "fixture"});
			return cleanup_receipt;
		}
	};

	[[nodiscard]] source_closure_manifest_descriptor
	manifest_descriptor(const source_closure_transfer_binding& value, const std::uint64_t bytes)
	{
		return {value.session_id,
				value.task_id,
				value.task_v4_digest,
				value.closure_id,
				value.closure_digest,
				value.manifest_digest,
				bytes,
				bytes,
				1U};
	}

	[[nodiscard]] source_closure_manifest_summary
	manifest_summary(const source_closure_transfer_binding& value, const std::uint64_t blob_bytes)
	{
		return {value.closure_id, value.closure_digest, value.manifest_digest, 1U, 1U, blob_bytes};
	}

	[[nodiscard]] source_closure_reject
	manifest_reject(const source_closure_transfer_binding& value,
					const std::string_view reason,
					const std::string_view cleanup_receipt)
	{
		return {value.session_id,
				value.task_id,
				"manifest-streaming",
				std::string{reason},
				{{"declared-manifest-bytes", 4U},
				 {"next-chunk-index", 0U},
				 {"received-manifest-bytes", 0U}},
				std::string{cleanup_receipt}};
	}

	void exercise_constants_and_digests()
	{
		require(source_closure_protocol_minor == 0U, "source-closure protocol minor drifted");
		require(source_closure_capability == "task-source-closure-v2",
				"source-closure capability drifted");
		for (std::uint16_t id = 24U; id <= 29U; ++id)
			require(is_source_closure_message_id(id), "source-closure message ID was not reserved");
		require(!is_source_closure_message_id(23U), "NG1 heartbeat ID was stolen");
		require(validate_source_closure_frame_header(24U, 0U), "valid frame header was rejected");
		require(!validate_source_closure_frame_header(23U, 0U),
				"heartbeat frame crossed the source seam");
		require(!validate_source_closure_frame_header(24U, 1U),
				"nonzero source frame flags were accepted");
		const std::array<std::string_view, 2U> capabilities{"task-input-chunks-v2",
															"task-source-closure-v2"};
		const std::array<std::string_view, 1U> missing_closure_capabilities{"task-input-chunks-v2"};
		const std::array<std::string_view, 1U> closure_capabilities{"task-source-closure-v2"};
		require(validate_source_closure_capability(0U, capabilities),
				"required source-closure capability was rejected");
		require(!validate_source_closure_capability(0U, missing_closure_capabilities),
				"capability absence was not rejected before payload admission");
		require(!validate_source_closure_capability(1U, closure_capabilities),
				"protocol downgrade was not rejected");

		const auto transfer = binding();
		const auto bytes = std::string{"hello"};
		const auto content =
			cxxlens::sdk::content_digest(std::as_bytes(std::span{bytes.data(), bytes.size()}));
		const std::array receipts{source_closure_blob_receipt{0U, content, 5U}};
		auto observed = source_closure_blob_receipts_digest(receipts);
		require(observed, "receipt digest failed");
		const auto expected_json =
			"[{\"blob_digest\":\"" + content + "\",\"blob_ordinal\":0,\"size_bytes\":5}]";
		const auto expected =
			cxxlens::sdk::semantic_digest("cxxlens.source-closure-blob-receipts.v1", expected_json);
		require(expected && *observed == *expected, "streamed receipt digest framing drifted");
	}

	void exercise_valid_transfer_and_rebinding()
	{
		auto outer = binding();
		test_authority authority{outer};
		test_sink sink;
		const auto manifest = std::string{"manifest"};
		const auto content = std::string{"hello"};
		sink.summary = manifest_summary(outer, content.size());
		source_closure_transfer_validator validator{outer, authority, sink};
		auto started = validator.begin_manifest(manifest_descriptor(outer, manifest.size()), 0U);
		require(started, "valid manifest descriptor was rejected");
		require(
			validator.manifest_chunk(
				{outer.session_id, outer.task_id, outer.manifest_digest, 0U, 0U, manifest.size()},
				std::as_bytes(std::span{manifest.data(), manifest.size()}),
				1U),
			"valid manifest chunk was rejected");
		require(validator.begin_blob({outer.session_id,
									  outer.task_id,
									  outer.closure_digest,
									  0U,
									  cxxlens::sdk::content_digest(
										  std::as_bytes(std::span{content.data(), content.size()})),
									  content.size(),
									  content.size(),
									  1U},
									 2U),
				"valid blob descriptor was rejected");
		const auto blob_digest =
			cxxlens::sdk::content_digest(std::as_bytes(std::span{content.data(), content.size()}));
		require(validator.blob_chunk(
					{outer.session_id, outer.task_id, 0U, blob_digest, 0U, 0U, content.size()},
					std::as_bytes(std::span{content.data(), content.size()}),
					3U),
				"valid blob chunk was rejected");
		auto receipts = source_closure_blob_receipts_digest(validator.blob_receipts());
		require(receipts, "valid transfer receipt digest failed");
		auto transfer_digest = source_closure_transfer_digest(outer, *receipts, 1U, content.size());
		require(transfer_digest, "valid transfer digest failed");
		require(validator.seal({outer.session_id,
								outer.task_id,
								outer.task_v4_digest,
								outer.manifest_digest,
								*receipts,
								1U,
								content.size(),
								outer.closure_digest,
								*transfer_digest},
							   4U),
				"valid source-closure seal was rejected");
		auto forged_ack = source_closure_ack{outer.session_id,
											 outer.task_id,
											 outer.closure_digest,
											 *transfer_digest,
											 typed("spool-receipt:semantic-v2:", 'f'),
											 sink.credentials.cleanup_owner};
		require(!validator.acknowledge(forged_ack, 5U) &&
					validator.state() == source_closure_transfer_state::closure_sealed &&
					validator.next_sequence() == 5U,
				"forged spool issuer was accepted or consumed a sequence");
		require(validator.acknowledge({outer.session_id,
									   outer.task_id,
									   outer.closure_digest,
									   *transfer_digest,
									   sink.credentials.spool_receipt,
									   sink.credentials.cleanup_owner},
									  5U),
				"valid source-closure acknowledgement was rejected");
		require(validator.state() == source_closure_transfer_state::closure_acknowledged,
				"valid transfer did not reach acknowledgement");
		require(authority.calls == 1U, "outer task authority was not revalidated before transfer");
		require(sink.cleanup_calls == 0U, "successful transfer cleaned up its sealed spool early");

		auto foreign = outer;
		foreign.task_v4_digest = typed("semantic-v2:sha256:", '9');
		foreign.task_id = "task:" + foreign.task_v4_digest;
		test_sink foreign_sink;
		test_authority foreign_authority{outer};
		source_closure_transfer_validator rebound{outer, foreign_authority, foreign_sink};
		require(!rebound.begin_manifest(manifest_descriptor(foreign, manifest.size()), 0U) &&
					rebound.state() == source_closure_transfer_state::task_v4_sealed,
				"foreign outer task was allowed to rebind the transfer");
		require(foreign_sink.manifest_begin_calls == 0U && foreign_sink.cleanup_calls == 0U,
				"foreign binding reached the spool before rejection");

		test_authority invalid_authority{outer};
		invalid_authority.valid = false;
		test_sink invalid_sink;
		source_closure_transfer_validator invalid_outer{outer, invalid_authority, invalid_sink};
		require(!invalid_outer.begin_manifest(manifest_descriptor(outer, manifest.size()), 0U) &&
					invalid_outer.state() == source_closure_transfer_state::rejected &&
					invalid_authority.calls == 1U && invalid_sink.manifest_begin_calls == 0U,
				"inherited task-v2.2/task-v4 authority was not revalidated before bytes");
	}

	void exercise_bounds_chunks_and_rejects()
	{
		auto outer = binding();
		test_authority authority{outer};
		test_sink sink;
		source_closure_transfer_validator validator{outer, authority, sink};
		require(!validator.begin_manifest(manifest_descriptor(outer, 40U * 1024U * 1024U + 1U), 0U),
				"manifest bound overflow was accepted");
		require(validator.state() == source_closure_transfer_state::task_v4_sealed,
				"pre-admission bound rejection mutated transfer state");

		const auto manifest = std::string{"data"};
		sink.summary = manifest_summary(outer, 1U);
		require(validator.begin_manifest(manifest_descriptor(outer, manifest.size()), 0U),
				"bounded manifest descriptor was rejected");
		require(!validator.manifest_chunk(
					{outer.session_id, outer.task_id, outer.manifest_digest, 0U, 0U, 1U},
					std::as_bytes(std::span{manifest.data(), manifest.size()}),
					1U),
				"byte-count mismatch was accepted");
		require(validator.next_sequence() == 1U,
				"malformed payload consumed a shared sequence before validation");
		require(
			!validator.manifest_chunk(
				{outer.session_id, outer.task_id, outer.manifest_digest, 0U, 0U, manifest.size()},
				std::as_bytes(std::span{manifest.data(), manifest.size()}),
				2U),
			"non-contiguous frame sequence was accepted");
		require(validator.next_sequence() == 1U,
				"non-contiguous frame sequence mutated validator state");

		test_sink blob_limit_sink;
		test_authority blob_limit_authority{outer};
		blob_limit_sink.summary =
			manifest_summary(outer, source_closure_transport_limits{}.maximum_blob_bytes + 1U);
		source_closure_transfer_validator blob_limit{outer, blob_limit_authority, blob_limit_sink};
		require(blob_limit.begin_manifest(manifest_descriptor(outer, manifest.size()), 0U),
				"blob-bound fixture manifest descriptor was rejected");
		require(
			blob_limit.manifest_chunk(
				{outer.session_id, outer.task_id, outer.manifest_digest, 0U, 0U, manifest.size()},
				std::as_bytes(std::span{manifest.data(), manifest.size()}),
				1U),
			"blob-bound fixture manifest chunk was rejected");
		require(!blob_limit.begin_blob({outer.session_id,
										outer.task_id,
										outer.closure_digest,
										0U,
										typed("sha256:", 'd'),
										source_closure_transport_limits{}.maximum_blob_bytes + 1U,
										1U,
										1U},
									   2U) &&
					blob_limit.state() == source_closure_transfer_state::manifest_validated &&
					blob_limit_sink.blob_begin_calls == 0U,
				"one-blob bound overflow reached the spool");

		test_sink reject_sink;
		test_authority reject_authority{outer};
		source_closure_transfer_validator rejected{outer, reject_authority, reject_sink};
		require(rejected.begin_manifest(manifest_descriptor(outer, manifest.size()), 0U),
				"reject fixture descriptor was rejected");
		auto reject =
			manifest_reject(outer, "source-closure.chunk-gap", reject_sink.cleanup_receipt);
		require(rejected.reject(reject, 1U), "phase-authentic reject was rejected");
		require(rejected.state() == source_closure_transfer_state::rejected &&
					reject_sink.cleanup_calls == 1U,
				"reject did not become terminal with one cleanup");

		test_sink invalid_reject_sink;
		test_authority invalid_reject_authority{outer};
		source_closure_transfer_validator invalid_reject{
			outer, invalid_reject_authority, invalid_reject_sink};
		require(invalid_reject.begin_manifest(manifest_descriptor(outer, manifest.size()), 0U),
				"invalid reject fixture descriptor was rejected");
		auto invalid =
			manifest_reject(outer, "source-closure.chunk-gap", invalid_reject_sink.cleanup_receipt);
		invalid.observed_counters.pop_back();
		require(!invalid_reject.reject(invalid, 1U) &&
					invalid_reject.state() == source_closure_transfer_state::manifest_open &&
					invalid_reject.next_sequence() == 1U,
				"phase-inauthentic reject was accepted or consumed incorrectly");

		test_sink cancel_sink;
		test_authority cancel_authority{outer};
		source_closure_transfer_validator cancelled{outer, cancel_authority, cancel_sink};
		require(cancelled.begin_manifest(manifest_descriptor(outer, manifest.size()), 0U),
				"cancel fixture descriptor was rejected");
		auto cancelled_reject = cancelled.cancel();
		require(cancelled_reject && cancelled_reject->reason_code == "source-closure.cancelled" &&
					cancelled.state() == source_closure_transfer_state::rejected &&
					cancel_sink.cleanup_calls == 1U,
				"cancel did not produce one live typed reject");

		test_sink timeout_sink;
		test_authority timeout_authority{outer};
		source_closure_transfer_validator timed_out{outer, timeout_authority, timeout_sink};
		require(timed_out.begin_manifest(manifest_descriptor(outer, manifest.size()), 0U),
				"timeout fixture descriptor was rejected");
		auto timeout_reject = timed_out.timeout();
		require(timeout_reject &&
					timeout_reject->reason_code == "source-closure.transfer-timeout" &&
					timed_out.state() == source_closure_transfer_state::rejected &&
					timeout_sink.cleanup_calls == 1U,
				"timeout did not produce one live typed reject");

		test_sink lost_sink;
		test_authority lost_authority{outer};
		source_closure_transfer_validator lost{outer, lost_authority, lost_sink};
		require(lost.connection_lost(true) &&
					lost.state() == source_closure_transfer_state::local_terminal &&
					lost.local_terminal() == source_closure_local_terminal::connection_lost &&
					lost.cancel_observed() && lost_sink.cleanup_calls == 1U,
				"connection-loss cleanup fabricated a peer reject or retained the spool");

		test_sink crash_sink;
		test_authority crash_authority{outer};
		source_closure_transfer_validator crashed{outer, crash_authority, crash_sink};
		require(crashed.worker_crashed(false) &&
					crashed.state() == source_closure_transfer_state::local_terminal &&
					crashed.local_terminal() == source_closure_local_terminal::worker_crashed &&
					!crashed.cancel_observed() && crash_sink.cleanup_calls == 1U,
				"worker crash did not become a local terminal");
	}
} // namespace

int main()
{
	exercise_constants_and_digests();
	exercise_valid_transfer_and_rebinding();
	exercise_bounds_chunks_and_rejects();
	return 0;
}
