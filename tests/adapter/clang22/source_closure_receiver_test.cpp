#include "llvm/clang22/source_closure_receiver.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/source_closure_spool.hpp"
#include "protocol_v2/closure.hpp"
#include "sdk/provider_protocol_v2_adapter.hpp"

namespace
{
	using namespace cxxlens::detail::clang22;
	using cxxlens::sdk::provider::frame;
	using cxxlens::sdk::provider::message_type;
	namespace protocol = ::cxxlens::protocol_v2;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	template <typename T>
	void require_result(const cxxlens::sdk::result<T>& result, const std::string_view message)
	{
		require(result.has_value(), message);
	}

	[[nodiscard]] std::string typed(const std::string_view prefix, const char digit)
	{
		return std::string{prefix} + std::string(64U, digit);
	}

	[[nodiscard]] std::span<const std::byte> bytes(const std::string& value)
	{
		return std::as_bytes(std::span{value.data(), value.size()});
	}

	class memory_source final : public source_closure_frame_source
	{
	  public:
		explicit memory_source(std::vector<std::byte> input) : input_{std::move(input)} {}

		cxxlens::sdk::result<std::size_t> read(const std::span<std::byte> destination) override
		{
			if (offset_ == input_.size())
				return std::size_t{};
			const auto count = std::min<std::size_t>(destination.size(), 17U);
			const auto available = std::min(count, input_.size() - offset_);
			std::ranges::copy(std::span{input_}.subspan(offset_, available), destination.begin());
			offset_ += available;
			return available;
		}

		[[nodiscard]] std::size_t bytes_read() const noexcept
		{
			return offset_;
		}

	  private:
		std::vector<std::byte> input_;
		std::size_t offset_{};
	};

	class memory_sink final : public source_closure_frame_sink
	{
	  public:
		cxxlens::sdk::result<void> write(const std::span<const std::byte> frame_bytes) override
		{
			output_.insert(output_.end(), frame_bytes.begin(), frame_bytes.end());
			return {};
		}

		std::vector<std::byte> output_;
	};

	class sequence_clock final : public source_closure_monotonic_clock
	{
	  public:
		explicit sequence_clock(std::vector<std::uint64_t> values) : values_{std::move(values)} {}

		cxxlens::sdk::result<std::uint64_t> now_ns() const override
		{
			if (index_ >= values_.size())
				return cxxlens::sdk::unexpected(cxxlens::sdk::error{
					"source-closure.test-clock-exhausted", "clock", "script-exhausted"});
			const auto value = values_[index_];
			++index_;
			return value;
		}

	  private:
		std::vector<std::uint64_t> values_;
		mutable std::size_t index_{};
	};

	class constant_clock final : public source_closure_monotonic_clock
	{
	  public:
		explicit constant_clock(const std::uint64_t value) : value_{value} {}

		cxxlens::sdk::result<std::uint64_t> now_ns() const override
		{
			return value_;
		}

	  private:
		std::uint64_t value_{};
	};

	class failing_source final : public source_closure_frame_source
	{
	  public:
		cxxlens::sdk::result<std::size_t> read(const std::span<std::byte>) override
		{
			return cxxlens::sdk::unexpected(
				cxxlens::sdk::error{"source-closure.channel-closed", "read", "peer-closed"});
		}
	};

	class authority final : public source_closure_task_v4_authority
	{
	  public:
		authority(std::string task, std::string digest)
			: task_{std::move(task)}, digest_{std::move(digest)}
		{
		}

		std::string task_;
		std::string digest_;
		std::size_t revalidate_calls{};

		std::string_view task_id() const noexcept override
		{
			return task_;
		}
		std::string_view task_v4_digest() const noexcept override
		{
			return digest_;
		}
		cxxlens::sdk::result<void> revalidate() const override
		{
			++const_cast<authority*>(this)->revalidate_calls;
			return {};
		}
	};

	struct fixture
	{
		source_closure_snapshot closure;
		source_closure_transfer_binding binding;
		std::string manifest_bytes;
		std::vector<std::byte> transcript;
	};

	[[nodiscard]] std::string manifest(const source_closure_snapshot& closure)
	{
		using cxxlens::detail::clang22::materialization::canonical_json;
		using cxxlens::detail::clang22::materialization::json_value;
		std::vector<json_value> members;
		for (const auto& item : closure.members)
		{
			json_value::object_type fields;
			fields.emplace("content_digest", json_value::string(item.content_digest).value());
			fields.emplace("encoding", json_value::string("utf8").value());
			fields.emplace("file_id", json_value::string(item.file_id).value());
			fields.emplace("logical_path", json_value::string(item.logical_path).value());
			fields.emplace("read_only", json_value::boolean(item.read_only));
			fields.emplace("role", json_value::string("main").value());
			fields.emplace("size_bytes", json_value::unsigned_integer(item.size_bytes));
			members.push_back(json_value::object(std::move(fields)).value());
		}
		std::vector<json_value> blobs;
		for (const auto& item : closure.blobs)
		{
			json_value::object_type fields;
			fields.emplace("content_digest", json_value::string(item.content_digest).value());
			fields.emplace("size_bytes", json_value::unsigned_integer(item.size_bytes));
			blobs.push_back(json_value::object(std::move(fields)).value());
		}
		json_value::object_type root;
		root.emplace("blobs", json_value::array(std::move(blobs)));
		root.emplace("closure_digest", json_value::string(closure.closure_digest).value());
		root.emplace("closure_id", json_value::string(closure.snapshot_id).value());
		root.emplace("members", json_value::array(std::move(members)));
		root.emplace("schema",
					 json_value::string(std::string{source_closure_manifest_schema}).value());
		return canonical_json(json_value::object(std::move(root)).value());
	}

	[[nodiscard]] fixture make_fixture()
	{
		const auto source = std::make_shared<const std::string>("int main() { return 0; }\n");
		auto closure = make_source_closure_snapshot({{"project://src/main.cpp",
													  source_closure_role::main,
													  source_closure_encoding::utf8,
													  source}});
		require_result(closure, "receiver fixture closure failed");
		fixture output{std::move(*closure), {}, {}, {}};
		output.manifest_bytes = manifest(output.closure);
		auto manifest_digest = cxxlens::sdk::semantic_digest(source_closure_manifest_digest_domain,
															 output.manifest_bytes);
		require_result(manifest_digest, "receiver fixture manifest digest failed");
		output.binding.session_id = typed("provider-session:sha256:", '1');
		output.binding.task_v4_digest = typed("semantic-v2:sha256:", '2');
		output.binding.task_id = "task:" + output.binding.task_v4_digest;
		output.binding.closure_id = output.closure.snapshot_id;
		output.binding.closure_digest = output.closure.closure_digest;
		output.binding.manifest_digest = *manifest_digest;

		const auto limits = protocol::closure_limits{};
		std::uint64_t sequence{};
		auto append = [&](const message_type type,
						  protocol::closure_control control,
						  std::span<const std::byte> payload = {})
		{
			auto encoded = protocol::encode_closure_control(
				static_cast<protocol::message_type>(static_cast<std::uint16_t>(type)),
				control,
				limits);
			require_result(encoded, "receiver fixture control encoding failed");
			frame value;
			value.type = type;
			value.stream_id = 7U;
			value.sequence = sequence++;
			value.control = std::move(*encoded);
			value.payload.assign(payload.begin(), payload.end());
			auto wire = cxxlens::sdk::provider::encode_frame(value);
			require_result(wire, "receiver fixture frame encoding failed");
			output.transcript.insert(output.transcript.end(), wire->begin(), wire->end());
		};

		append(message_type::source_closure_manifest,
			   protocol::source_closure_manifest_descriptor{
				   cxxlens::protocol_v2::manifest_kind::descriptor,
				   output.binding.session_id,
				   output.binding.task_id,
				   output.binding.task_v4_digest,
				   output.binding.closure_id,
				   output.binding.closure_digest,
				   output.binding.manifest_digest,
				   output.manifest_bytes.size(),
				   output.manifest_bytes.size(),
				   1U});
		append(message_type::source_closure_manifest,
			   protocol::source_closure_manifest_chunk{cxxlens::protocol_v2::manifest_kind::chunk,
													   output.binding.session_id,
													   output.binding.task_id,
													   output.binding.manifest_digest,
													   0U,
													   0U,
													   output.manifest_bytes.size()},
			   bytes(output.manifest_bytes));
		const auto& blob = output.closure.blobs.front();
		append(message_type::source_closure_blob,
			   protocol::source_closure_blob_descriptor{output.binding.session_id,
														output.binding.task_id,
														output.binding.closure_digest,
														0U,
														blob.content_digest,
														blob.size_bytes,
														blob.size_bytes,
														1U});
		append(message_type::source_closure_chunk,
			   protocol::source_closure_chunk{output.binding.session_id,
											  output.binding.task_id,
											  0U,
											  blob.content_digest,
											  0U,
											  0U,
											  blob.size_bytes},
			   bytes(*blob.content));
		const std::array receipts{
			source_closure_blob_receipt{0U, blob.content_digest, blob.size_bytes}};
		auto receipts_digest = source_closure_blob_receipts_digest(receipts);
		require_result(receipts_digest, "receiver fixture receipt digest failed");
		auto transfer_digest =
			source_closure_transfer_digest(output.binding, *receipts_digest, 1U, blob.size_bytes);
		require_result(transfer_digest, "receiver fixture transfer digest failed");
		append(message_type::source_closure_seal,
			   protocol::source_closure_seal{output.binding.session_id,
											 output.binding.task_id,
											 output.binding.task_v4_digest,
											 output.binding.manifest_digest,
											 *receipts_digest,
											 1U,
											 blob.size_bytes,
											 output.binding.closure_digest,
											 *transfer_digest});
		return output;
	}

	template <typename Mutator>
	[[nodiscard]] std::vector<std::byte> first_frame_with(const fixture& input, Mutator&& mutate)
	{
		require(input.transcript.size() >= protocol::fixed_header_bytes,
				"receiver fixture had no complete frame header");
		const std::span<const std::byte, protocol::fixed_header_bytes> header{
			input.transcript.data(), protocol::fixed_header_bytes};
		auto prepared =
			cxxlens::sdk::provider::detail::prepare_provider_protocol_v2_frame(header, {});
		require_result(prepared, "receiver fixture header preparation failed");
		const auto total = protocol::fixed_header_bytes + prepared->body_resident_bytes();
		require(total <= input.transcript.size(), "receiver fixture first frame was truncated");
		auto decoded = cxxlens::sdk::provider::decode_frame(
			std::span<const std::byte>{input.transcript}.first(total));
		require_result(decoded, "receiver fixture first frame decode failed");
		std::forward<Mutator>(mutate)(*decoded);
		auto encoded = cxxlens::sdk::provider::encode_frame(*decoded);
		require_result(encoded, "receiver fixture first frame re-encode failed");
		return std::move(*encoded);
	}

	void positive_transfer()
	{
		auto input = make_fixture();
		memory_source source{input.transcript};
		memory_sink sink;
		authority task{input.binding.task_id, input.binding.task_v4_digest};
		constant_clock deterministic_clock{0U};
		auto result = receive_source_closure_frames(
			source, sink, {input.binding, &task, 7U, 16'384U, {}, {}, &deterministic_clock});
		require_result(result, "receiver rejected valid source closure");
		require(result->snapshot.snapshot_id == input.closure.snapshot_id &&
					result->snapshot.closure_digest == input.closure.closure_digest &&
					result->snapshot.members.size() == input.closure.members.size() &&
					result->snapshot.blobs.size() == input.closure.blobs.size() &&
					result->snapshot.blobs.front().content != nullptr &&
					*result->snapshot.blobs.front().content == *input.closure.blobs.front().content,
				"receiver changed closure snapshot");
		require(task.revalidate_calls == 1U, "receiver skipped inherited authority revalidation");
		require(!sink.output_.empty(), "receiver did not emit source-closure ACK");
		auto ack = cxxlens::sdk::provider::decode_frame(sink.output_);
		require_result(ack, "receiver ACK was not a valid provider frame");
		require(ack->type == message_type::source_closure_ack && ack->sequence == 5U,
				"receiver ACK had the wrong sequence");
		auto decoded = protocol::decode_closure_control(protocol::message_type::source_closure_ack,
														ack->control);
		require_result(decoded, "receiver ACK control was not canonical");
		const auto* value = std::get_if<protocol::source_closure_ack>(&*decoded);
		require(value != nullptr && value->spool_receipt == result->credentials.spool_receipt &&
					value->cleanup_owner == result->credentials.cleanup_owner &&
					value->transfer_digest == result->credentials.transfer_digest,
				"receiver ACK was not bound to the sealed spool");
		require(result->relay != nullptr &&
					result->relay->terminal() == source_closure_relay_terminal::sealed,
				"receiver did not retain sealed relay ownership after ACK");
		auto crashed = result->relay->worker_crashed(false);
		require_result(crashed, "worker-crash relay cleanup failed");
		require(result->relay->terminal() == source_closure_relay_terminal::worker_crashed &&
					!result->relay->cancel_observed(),
				"worker-crash relay terminal was not recorded");
	}

	void truncated_transfer()
	{
		auto input = make_fixture();
		input.transcript.resize(input.transcript.size() - 104U - 0U);
		memory_source source{std::move(input.transcript)};
		memory_sink sink;
		authority task{input.binding.task_id, input.binding.task_v4_digest};
		auto result = receive_source_closure_frames(
			source, sink, {input.binding, &task, 7U, 16'384U, {}, {}});
		require(!result && result.error().code == "source-closure.truncated-stream",
				"truncated source closure did not fail closed");
		require(sink.output_.empty(), "truncated source closure emitted an ACK");
	}

	void frame_header_authority_precedes_body_allocation_and_read()
	{
		auto input = make_fixture();
		auto run = [&](std::vector<std::byte> wire,
					   const std::string_view expected_code,
					   const std::string_view message)
		{
			memory_source source{std::move(wire)};
			memory_sink sink;
			authority task{input.binding.task_id, input.binding.task_v4_digest};
			auto result = receive_source_closure_frames(
				source, sink, {input.binding, &task, 7U, 16'384U, {}, {}});
			require(!result && result.error().code == expected_code, message);
			require(source.bytes_read() == protocol::fixed_header_bytes,
					"receiver consumed a rejected frame body before header authority");
			require(sink.output_.empty(), "receiver emitted an ACK for a rejected frame header");
		};

		run(first_frame_with(input,
							 [](frame& value)
							 {
								 value.stream_id = 8U;
							 }),
			"source-closure.session-binding-mismatch",
			"foreign stream was not rejected from the prepared header");
		run(first_frame_with(input,
							 [](frame& value)
							 {
								 value.sequence = 1U;
							 }),
			"source-closure.protocol-state-invalid",
			"foreign sequence was not rejected from the prepared header");
		run(first_frame_with(input,
							 [](frame& value)
							 {
								 value.type = message_type::heartbeat;
							 }),
			"source-closure.protocol-state-invalid",
			"non-closure channel message was not rejected from the prepared header");
	}

	void resident_contract_is_hard_and_body_is_read_once()
	{
		auto input = make_fixture();
		const std::span<const std::byte, protocol::fixed_header_bytes> header{
			input.transcript.data(), protocol::fixed_header_bytes};
		auto prepared =
			cxxlens::sdk::provider::detail::prepare_provider_protocol_v2_frame(header, {});
		require_result(prepared, "resident fixture header preparation failed");
		require(prepared->body_resident_bytes() > 0U, "resident fixture had an empty frame body");

		source_closure_transport_limits narrow;
		narrow.maximum_resident_transport_bytes = prepared->body_resident_bytes() - 1U;
		memory_source narrow_source{first_frame_with(input,
													 [](frame&)
													 {
													 })};
		memory_sink narrow_sink;
		authority narrow_authority{input.binding.task_id, input.binding.task_v4_digest};
		auto narrowed = receive_source_closure_frames(
			narrow_source,
			narrow_sink,
			{input.binding, &narrow_authority, 7U, 16'384U, narrow, {}});
		require(!narrowed && narrowed.error().code == "source-closure.limit-exceeded" &&
					narrow_source.bytes_read() == protocol::fixed_header_bytes,
				"frame body bypassed the narrowed resident ledger");

		source_closure_transport_limits raised;
		++raised.maximum_resident_transport_bytes;
		memory_source raised_source{input.transcript};
		memory_sink raised_sink;
		authority raised_authority{input.binding.task_id, input.binding.task_v4_digest};
		auto raised_result = receive_source_closure_frames(
			raised_source,
			raised_sink,
			{input.binding, &raised_authority, 7U, 16'384U, raised, {}});
		require(!raised_result && raised_result.error().code == "source-closure.limit-exceeded" &&
					raised_source.bytes_read() == 0U,
				"caller raised the fixed resident transport contract");

		auto truncated_wire = first_frame_with(input,
											   [](frame&)
											   {
											   });
		truncated_wire.pop_back();
		const auto truncated_bytes = truncated_wire.size();
		memory_source truncated_source{std::move(truncated_wire)};
		memory_sink truncated_sink;
		authority truncated_authority{input.binding.task_id, input.binding.task_v4_digest};
		auto truncated = receive_source_closure_frames(
			truncated_source,
			truncated_sink,
			{input.binding, &truncated_authority, 7U, 16'384U, {}, {}});
		require(!truncated && truncated.error().code == "source-closure.truncated-stream" &&
					truncated_source.bytes_read() == truncated_bytes,
				"partial final-owned frame body did not fail closed");
	}

	void initial_credit_counts_every_wire_header()
	{
		constexpr source_closure_transport_limits limits{};
		auto credit = source_closure_receiver_initial_credit(limits);
		require_result(credit, "receiver initial credit calculation failed");
		constexpr std::uint64_t expected_frames = 8'282U;
		constexpr std::uint64_t expected_missing_headers =
			expected_frames * protocol::fixed_header_bytes;
		constexpr std::uint64_t expected_bytes = limits.maximum_task_spool_bytes +
			expected_frames * (protocol::max_control_bytes + protocol::fixed_header_bytes);
		require(credit->frames == expected_frames && credit->bytes == expected_bytes &&
					expected_missing_headers == 861'328U,
				"receiver credit omitted complete-frame header bytes");

		protocol::credit_window generic{{credit->bytes, credit->frames}};
		protocol::frame maximum_frame;
		maximum_frame.control.resize(protocol::max_control_bytes);
		const auto payload_per_frame = limits.maximum_task_spool_bytes / expected_frames;
		const auto payload_remainder = limits.maximum_task_spool_bytes % expected_frames;
		maximum_frame.payload.resize(static_cast<std::size_t>(payload_per_frame));
		for (std::uint64_t index = 0U; index < expected_frames; ++index)
		{
			maximum_frame.payload.resize(static_cast<std::size_t>(
				payload_per_frame + (index + 1U == expected_frames ? payload_remainder : 0U)));
			auto consumed = generic.consume(maximum_frame);
			require_result(consumed, "generic Protocol 2 credit rejected receiver bound");
		}
		require(generic.available().bytes == 0U && generic.available().frames == 0U,
				"receiver and generic Protocol 2 credit units diverged");

		auto frame_overflow = limits;
		frame_overflow.maximum_manifest_chunks = std::numeric_limits<std::uint64_t>::max();
		auto rejected_frames = source_closure_receiver_initial_credit(frame_overflow);
		require(!rejected_frames &&
					rejected_frames.error().code == "source-closure.limit-exceeded" &&
					rejected_frames.error().field == "credit.frames",
				"receiver did not reject frame-credit arithmetic overflow");

		auto byte_overflow = limits;
		byte_overflow.maximum_task_spool_bytes = std::numeric_limits<std::uint64_t>::max();
		auto rejected_bytes = source_closure_receiver_initial_credit(byte_overflow);
		require(!rejected_bytes && rejected_bytes.error().code == "source-closure.limit-exceeded" &&
					rejected_bytes.error().field == "credit.bytes",
				"receiver did not reject byte-credit arithmetic overflow");
	}

	void liveness_and_connection_terminals()
	{
		auto input = make_fixture();
		memory_sink timeout_sink;
		authority timeout_authority{input.binding.task_id, input.binding.task_v4_digest};
		sequence_clock timeout_clock{{0U, 5U}};
		memory_source stalled_source{std::vector<std::byte>{}};
		auto timeout = receive_source_closure_frames(
			stalled_source,
			timeout_sink,
			{input.binding, &timeout_authority, 7U, 16'384U, {}, {}, &timeout_clock, 5U});
		require(!timeout && timeout.error().code == "source-closure.transfer-timeout" &&
					timeout_sink.output_.empty(),
				"stalled source closure did not produce a typed timeout without ACK");

		memory_sink late_sink;
		authority late_authority{input.binding.task_id, input.binding.task_v4_digest};
		sequence_clock late_clock{{0U, 0U, 5U}};
		memory_source late_source{input.transcript};
		auto late = receive_source_closure_frames(
			late_source,
			late_sink,
			{input.binding, &late_authority, 7U, 16'384U, {}, {}, &late_clock, 5U});
		require(!late && late.error().code == "source-closure.transfer-timeout" &&
					late_sink.output_.empty(),
				"frame arriving at the absolute deadline was accepted or ACKed");

		memory_sink backwards_sink;
		authority backwards_authority{input.binding.task_id, input.binding.task_v4_digest};
		sequence_clock backwards_clock{{10U, 9U}};
		memory_source backwards_source{std::vector<std::byte>{}};
		auto backwards = receive_source_closure_frames(
			backwards_source,
			backwards_sink,
			{input.binding, &backwards_authority, 7U, 16'384U, {}, {}, &backwards_clock, 10U});
		require(!backwards && backwards.error().code == "source-closure.channel-clock-invalid" &&
					backwards.error().detail == "backwards" && backwards_sink.output_.empty(),
				"backwards receiver clock was not rejected before read/ACK");

		memory_sink overflow_sink;
		authority overflow_authority{input.binding.task_id, input.binding.task_v4_digest};
		sequence_clock overflow_clock{{std::numeric_limits<std::uint64_t>::max()}};
		memory_source overflow_source{std::vector<std::byte>{}};
		auto overflow = receive_source_closure_frames(
			overflow_source,
			overflow_sink,
			{input.binding, &overflow_authority, 7U, 16'384U, {}, {}, &overflow_clock, 1U});
		require(!overflow && overflow.error().code == "source-closure.channel-clock-invalid" &&
					overflow.error().detail == "deadline-overflow" && overflow_sink.output_.empty(),
				"receiver deadline overflow was not rejected before read/ACK");

		memory_sink failed_clock_sink;
		authority failed_clock_authority{input.binding.task_id, input.binding.task_v4_digest};
		sequence_clock failed_clock{{}};
		memory_source failed_clock_source{std::vector<std::byte>{}};
		auto failed_clock_result = receive_source_closure_frames(
			failed_clock_source,
			failed_clock_sink,
			{input.binding, &failed_clock_authority, 7U, 16'384U, {}, {}, &failed_clock, 1U});
		require(!failed_clock_result &&
					failed_clock_result.error().code == "source-closure.channel-clock-invalid" &&
					failed_clock_result.error().detail == "script-exhausted" &&
					failed_clock_sink.output_.empty(),
				"receiver clock port failure was not propagated before read/ACK");

		memory_sink cancel_sink;
		authority cancel_authority{input.binding.task_id, input.binding.task_v4_digest};
		std::stop_source cancellation;
		cancellation.request_stop();
		memory_source cancelled_source{input.transcript};
		auto cancelled = receive_source_closure_frames(cancelled_source,
													   cancel_sink,
													   {input.binding,
														&cancel_authority,
														7U,
														16'384U,
														{},
														cancellation.get_token(),
														nullptr,
														5U});
		require(!cancelled && cancelled.error().code == "source-closure.cancelled" &&
					cancel_sink.output_.empty(),
				"pre-cancelled source closure did not fail closed without ACK");

		memory_sink connection_sink;
		authority connection_authority{input.binding.task_id, input.binding.task_v4_digest};
		failing_source disconnected_source;
		auto disconnected = receive_source_closure_frames(
			disconnected_source,
			connection_sink,
			{input.binding, &connection_authority, 7U, 16'384U, {}, {}});
		require(!disconnected && disconnected.error().code == "source-closure.channel-closed" &&
					connection_sink.output_.empty(),
				"connection loss did not remain a local typed channel terminal");
	}

	void complete_frame_progress_starts_the_next_absolute_interval()
	{
		auto input = make_fixture();
		memory_source source{input.transcript};
		memory_sink sink;
		authority task{input.binding.task_id, input.binding.task_v4_digest};
		sequence_clock clock{{0U, 1U, 4U, 8U, 8U, 12U, 12U, 16U, 16U, 20U, 20U}};
		auto result = receive_source_closure_frames(
			source, sink, {input.binding, &task, 7U, 16'384U, {}, {}, &clock, 5U});
		require_result(result, "complete frame progress did not start the next deadline interval");
		require(!sink.output_.empty(), "progress-reset transfer did not emit its ACK");
		auto crashed = result->relay->worker_crashed(false);
		require_result(crashed, "progress-reset fixture cleanup failed");
	}
} // namespace

int main()
{
	positive_transfer();
	truncated_transfer();
	frame_header_authority_precedes_body_allocation_and_read();
	resident_contract_is_hard_and_body_is_read_once();
	initial_credit_counts_every_wire_header();
	liveness_and_connection_terminals();
	complete_frame_progress_starts_the_next_absolute_interval();
	return 0;
}
