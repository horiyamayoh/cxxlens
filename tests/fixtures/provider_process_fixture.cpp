#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/sdk/provider.hpp>
#include <fcntl.h>
#include <sys/socket.h>

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;

	constexpr std::string_view provider_id = "company.test.process-provider";

	template <std::unsigned_integral T>
	void append_big_endian(std::vector<std::byte>& output, const T value)
	{
		for (std::size_t index = sizeof(T); index > 0U; --index)
			output.push_back(static_cast<std::byte>(value >> ((index - 1U) * 8U)));
	}

	[[nodiscard]] std::vector<std::byte> control(const std::string_view text)
	{
		std::vector<std::byte> output;
		if (text.size() < 24U)
			output.push_back(static_cast<std::byte>(0x60U | text.size()));
		else if (text.size() <= std::numeric_limits<std::uint8_t>::max())
		{
			output.push_back(std::byte{0x78});
			output.push_back(static_cast<std::byte>(text.size()));
		}
		else
		{
			output.push_back(std::byte{0x79});
			append_big_endian(output, static_cast<std::uint16_t>(text.size()));
		}
		for (const auto byte : text)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	[[nodiscard]] detached_row output_row()
	{
		using relation = cxxlens::company::relations::lock_acquire;
		relation::builder builder;
		if (!builder.set<relation::acquire>(
				detached_cell::typed("company_lock_acquire_id", "lock-acquire:1")) ||
			!builder.set<relation::lock>(detached_cell::typed("company_lock_id", "lock:1")) ||
			!builder.set<relation::source>(detached_cell::typed("source_span_id", "span:1")) ||
			!builder.set<relation::mode>(
				detached_cell{{scalar_kind::open_symbol, "company.lock-mode/1", false},
							  cell_state::present,
							  scalar_value{std::string{"exclusive"}},
							  std::nullopt}) ||
			!builder.set<relation::ordinal>(detached_cell::unsigned_integer(0U)))
			std::exit(EXIT_FAILURE);
		auto row = std::move(builder).finish();
		if (!row)
			std::exit(EXIT_FAILURE);
		return std::move(*row);
	}

	class stdout_sink final : public frame_sink
	{
	  public:
		result<void> write(const std::span<const std::byte> bytes) override
		{
			std::cout.write(reinterpret_cast<const char*>(bytes.data()),
							static_cast<std::streamsize>(bytes.size()));
			return std::cout.good()
				? result<void>{}
				: cxxlens::sdk::unexpected(error{"provider.fixture-write", "stdout", {}});
		}
	};
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	if (argument_count != 2)
		return EXIT_FAILURE;
	const std::string_view mode{arguments[1]};
	std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
	std::vector<std::byte> bytes;
	bytes.reserve(input.size());
	for (const auto byte : input)
		bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
	protocol_limits input_limits;
	input_limits.maximum_minor = std::numeric_limits<std::uint16_t>::max();
	auto frames = decode_frame_stream(bytes, input_limits);
	if (!frames || frames->size() != 5U || frames->at(0U).type != message_type::hello_ack ||
		frames->at(1U).type != message_type::schema_negotiate ||
		frames->at(2U).type != message_type::open_task ||
		frames->at(3U).type != message_type::credit || frames->at(4U).type != message_type::close)
		return EXIT_FAILURE;
	auto schema_control = decode_schema_negotiate_metadata(frames->at(1U).control);
	auto task_control = decode_open_task_metadata(frames->at(2U).control);
	auto credit_control = decode_credit_metadata(frames->at(3U).control);
	auto close_control = decode_close_metadata(frames->at(4U).control);
	if (!schema_control || !task_control || !credit_control || !close_control ||
		schema_control->protocol_schema.empty() || credit_control->bytes == 0U ||
		credit_control->frames == 0U || close_control->task_id != task_control->task_id)
		return EXIT_FAILURE;
	const std::string task_id{task_control->task_id};

	if (mode == "crash")
		(void)::raise(SIGSEGV);
	if (mode == "timeout")
		std::this_thread::sleep_for(std::chrono::seconds{5});
	if (mode == "output-limit")
	{
		std::cout << std::string(1024U * 1024U, 'x');
		return EXIT_SUCCESS;
	}
	if (mode == "malformed")
	{
		std::cout << "not-a-provider-frame";
		return EXIT_SUCCESS;
	}

	stdout_sink sink;
	protocol_limits output_limits;
	output_limits.protocol_major = frames->front().protocol_major;
	output_limits.minimum_minor = frames->front().protocol_minor;
	output_limits.maximum_minor = frames->front().protocol_minor;
	protocol_writer writer{sink, output_limits};
	writer.grant_credit({64U * 1024U * 1024U, 65536U});
	auto identity = decode_control_text(frames->at(0U).control).value();
	if (mode == "wrong-identity")
		identity.replace(identity.find(provider_id), provider_id.size(), "company.test.other");
	const auto hello_flags =
		mode == "bad-eos" ? static_cast<std::uint16_t>(frame_flag::end_of_stream) : std::uint16_t{};
	if (!writer.send(message_type::hello, control(identity), {}, hello_flags))
		return EXIT_FAILURE;
	if (mode == "minimal")
	{
		auto complete = encode_task_complete_metadata({task_id});
		return complete && writer.send(message_type::task_complete, *complete) ? EXIT_SUCCESS
																			   : EXIT_FAILURE;
	}
	if (!writer.send(message_type::schema_negotiate, frames->at(1U).control))
		return EXIT_FAILURE;
	if (mode == "failed" || mode == "failure-success" || mode == "failure-unknown")
	{
		const auto reason = mode == "failure-success"
			? "provider.success"
			: (mode == "failure-unknown" ? "provider.unknown-reason" : "provider.schema-invalid");
		auto failed = encode_task_failed_metadata({reason, task_id, "fixture"});
		return failed && writer.send(message_type::task_failed, *failed) ? EXIT_SUCCESS
																		 : EXIT_FAILURE;
	}
	if (mode == "invalid-utf8")
	{
		const std::array invalid_control{std::byte{0x61}, std::byte{0x80}};
		if (!writer.send(message_type::task_accepted, invalid_control))
			return EXIT_FAILURE;
		auto failed = encode_task_failed_metadata({"provider.schema-invalid", task_id, "fixture"});
		return failed && writer.send(message_type::task_failed, *failed) ? EXIT_SUCCESS
																		 : EXIT_FAILURE;
	}
	if (mode != "missing-accepted")
	{
		auto accepted = encode_task_accepted_metadata(
			{std::string{provider_id}, "1.0.0", mode == "wrong-task" ? "other-task" : task_id});
		if (!accepted || !writer.send(message_type::task_accepted, *accepted))
			return EXIT_FAILURE;
	}
	if (mode == "missing-accepted")
	{
		auto complete = encode_task_complete_metadata({task_id});
		return complete && writer.send(message_type::task_complete, *complete) ? EXIT_SUCCESS
																			   : EXIT_FAILURE;
	}
	if (mode == "nul-control")
	{
		constexpr char nul_code[]{"provider.schema-invalid\0suffix"};
		auto failed = encode_task_failed_metadata(
			{std::string{nul_code, sizeof(nul_code) - 1U}, task_id, "fixture"});
		return failed && writer.send(message_type::task_failed, *failed) ? EXIT_SUCCESS
																		 : EXIT_FAILURE;
	}
	if (mode == "provider-credit" || mode == "provider-open-task" || mode == "provider-batch-ack")
	{
		const auto type = mode == "provider-credit"
			? message_type::credit
			: (mode == "provider-open-task" ? message_type::open_task : message_type::batch_ack);
		if (!writer.send(type, control("provider-forbidden")))
			return EXIT_FAILURE;
		auto complete = encode_task_complete_metadata({task_id});
		return complete && writer.send(message_type::task_complete, *complete) ? EXIT_SUCCESS
																			   : EXIT_FAILURE;
	}
	if (mode == "optional-extension" &&
		!writer.send(static_cast<message_type>(65000U),
					 control("company.optional-extension"),
					 {},
					 static_cast<std::uint16_t>(frame_flag::optional_extension)))
		return EXIT_FAILURE;
	if (mode == "network-check")
	{
		const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
		if (descriptor >= 0)
			return EXIT_FAILURE;
	}
	if (mode == "fd-clean")
		for (int descriptor = 3; descriptor < 1024; ++descriptor)
		{
			errno = 0;
			if (::fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF)
				return EXIT_FAILURE;
		}

	const auto& schema = cxxlens::company::relations::lock_acquire::descriptor();
	const auto descriptor =
		std::string{mode == "unknown-descriptor" ? "company.unknown.v1" : schema.id};
	const auto descriptor_digest = mode == "unknown-descriptor"
		? std::string{"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"}
		: schema.descriptor_digest;
	auto begin = encode_batch_begin_metadata(
		{task_id, descriptor, descriptor_digest, "dependency-1", "atomic-1", "batch-1"});
	if (!begin || !writer.send(message_type::batch_begin, *begin))
		return EXIT_FAILURE;
	if (mode == "unsealed-batch")
	{
		auto complete = encode_task_complete_metadata({task_id});
		return complete && writer.send(message_type::task_complete, *complete) ? EXIT_SUCCESS
																			   : EXIT_FAILURE;
	}
	const auto row = output_row();
	std::vector<batch_column_summary> summaries;
	std::vector<std::string> chunk_digests;
	std::vector<encoded_column_chunk> encoded_chunks;
	for (std::size_t index = 0U; index < schema.columns.size(); ++index)
	{
		const auto& column = schema.columns[index];
		const auto encoding = column.type.scalar == scalar_kind::boolean
			? "fixed-width-bool-u8"
			: (column.type.scalar == scalar_kind::signed_integer
				   ? "fixed-width-i64-le"
				   : (column.type.scalar == scalar_kind::unsigned_integer
						  ? "fixed-width-u64-le"
						  : ((column.type.scalar == scalar_kind::open_symbol ||
							  column.type.scalar == scalar_kind::closed_symbol)
								 ? "dictionary-index-u32-le"
								 : ((column.type.scalar == scalar_kind::bytes ||
									 column.type.scalar == scalar_kind::set)
										? "bytes-offsets-u32-le"
										: "utf8-offsets-u32-le"))));
		const auto cell = row.cells.contains(column.id) ? row.cells.at(column.id)
														: detached_cell::absent(column.type);
		column_chunk_record chunk{task_id,
								  "dependency-1",
								  "atomic-1",
								  "batch-1",
								  schema.id,
								  schema.descriptor_digest,
								  column.id,
								  0U,
								  1U,
								  0U,
								  encoding,
								  {cell},
								  {}};
		auto encoded = encode_column_chunk(chunk, column);
		if (!encoded)
			return EXIT_FAILURE;
		if (mode == "bad-column" && index == 0U)
			encoded->payload.back() ^= std::byte{1U};
		summaries.push_back({column.id, encoded->payload.size(), 1U});
		chunk_digests.push_back(encoded->chunk_digest);
		encoded_chunks.push_back(std::move(*encoded));
	}
	if (mode == "reordered-column")
		std::swap(encoded_chunks[0U], encoded_chunks[1U]);
	for (const auto& encoded : encoded_chunks)
		if (!writer.send(message_type::column_chunk, encoded.control, encoded.payload))
			return EXIT_FAILURE;
	columnar_batch_end terminal{task_id,
								"dependency-1",
								"atomic-1",
								"batch-1",
								schema.id,
								schema.descriptor_digest,
								1U,
								std::move(summaries),
								std::move(chunk_digests),
								{}};
	terminal.batch_digest = columnar_batch_digest(terminal);
	auto encoded_terminal = encode_columnar_batch_end(terminal);
	if (!encoded_terminal)
		return EXIT_FAILURE;
	if (mode == "inconsistent-batch")
		encoded_terminal->payload.back() ^= std::byte{1U};
	if (mode == "column-length-mismatch")
		encoded_terminal->payload[10U + schema.columns.front().id.size()] ^= std::byte{1U};
	if (!writer.send(message_type::batch_end, encoded_terminal->control, encoded_terminal->payload))
		return EXIT_FAILURE;
	const std::array coverage{coverage_unit{mode == "incomplete-coverage" ? "project" : "task",
											mode == "incomplete-coverage" ? "catalog" : task_id,
											"covered",
											{}}};
	const std::span<const unresolved_item> unresolved;
	const std::span<const evidence_item> evidence;
	auto coverage_control = encode_coverage_metadata(coverage);
	auto unresolved_control = encode_unresolved_metadata(unresolved);
	auto evidence_control = encode_evidence_metadata(evidence);
	const auto terminal_flags = mode == "success-eos"
		? static_cast<std::uint16_t>(frame_flag::end_of_stream)
		: std::uint16_t{};
	auto complete_control =
		encode_task_complete_metadata({mode == "wrong-complete-task" ? "other-task" : task_id});
	const std::span<const std::byte> final_control = mode == "missing-complete-control"
		? std::span<const std::byte>{}
		: std::span<const std::byte>{*complete_control};
	if (!coverage_control || !unresolved_control || !evidence_control || !complete_control ||
		!writer.send(message_type::coverage_chunk, *coverage_control) ||
		!writer.send(message_type::unresolved_chunk, *unresolved_control) ||
		!writer.send(message_type::progress, *evidence_control) ||
		!writer.send(message_type::task_complete, final_control, {}, terminal_flags))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
