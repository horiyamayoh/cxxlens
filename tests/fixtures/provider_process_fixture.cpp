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

	[[nodiscard]] std::vector<std::byte> payload(const std::string_view text)
	{
		const auto bytes = std::as_bytes(std::span{text});
		return {bytes.begin(), bytes.end()};
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
	auto frames = decode_frame_stream(bytes);
	if (!frames || frames->size() != 5U || frames->at(0U).type != message_type::hello_ack ||
		frames->at(1U).type != message_type::schema_negotiate ||
		frames->at(2U).type != message_type::open_task ||
		frames->at(3U).type != message_type::credit)
		return EXIT_FAILURE;

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
	protocol_writer writer{sink};
	writer.grant_credit({64U * 1024U * 1024U, 65536U});
	auto identity = decode_control_text(frames->at(0U).control).value();
	if (mode == "wrong-identity")
		identity.replace(identity.find(provider_id), provider_id.size(), "company.test.other");
	if (!writer.send(message_type::hello, control(identity)))
		return EXIT_FAILURE;
	if (mode == "minimal")
		return writer.send(message_type::task_complete, control("task-1|complete")) ? EXIT_SUCCESS
																					: EXIT_FAILURE;
	if (!writer.send(message_type::schema_negotiate, frames->at(1U).control))
		return EXIT_FAILURE;
	if (mode == "failed")
		return writer.send(message_type::task_failed, control("provider.schema-invalid|fixture"))
			? EXIT_SUCCESS
			: EXIT_FAILURE;
	if (mode != "missing-accepted" &&
		!writer.send(message_type::task_accepted,
					 control(std::string{provider_id} + "|1.0.0|" +
							 (mode == "wrong-task" ? "other-task" : "task-1"))))
		return EXIT_FAILURE;
	if (mode == "missing-accepted")
		return writer.send(message_type::task_complete, control("task-1|complete")) ? EXIT_SUCCESS
																					: EXIT_FAILURE;
	if (mode == "provider-credit" || mode == "provider-open-task" || mode == "provider-batch-ack")
	{
		const auto type = mode == "provider-credit"
			? message_type::credit
			: (mode == "provider-open-task" ? message_type::open_task : message_type::batch_ack);
		if (!writer.send(type, control("provider-forbidden")))
			return EXIT_FAILURE;
		return writer.send(message_type::task_complete, control("task-1|complete")) ? EXIT_SUCCESS
																					: EXIT_FAILURE;
	}
	if (mode == "network-check")
	{
		const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
		if (descriptor >= 0)
			return EXIT_FAILURE;
	}

	const auto& schema = cxxlens::company::relations::lock_acquire::descriptor();
	const auto descriptor =
		std::string{mode == "unknown-descriptor" ? "company.unknown.v1" : schema.id};
	const auto descriptor_digest = mode == "unknown-descriptor"
		? std::string{"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"}
		: schema.descriptor_digest;
	const auto row = mode == "unknown-descriptor"
		? std::string{"{\"cells\":{},\"descriptor_id\":\"company.unknown.v1\"}"}
		: output_row().canonical_form();
	const auto initial = *semantic_digest("cxxlens.provider-batch.v1", descriptor_digest);
	const auto rolling = *semantic_digest("cxxlens.provider-batch.v1", initial + "\n" + row);
	if (!writer.send(
			message_type::batch_begin,
			control(descriptor + "|" + descriptor_digest + "|dependency-1|atomic-1|batch-1")))
		return EXIT_FAILURE;
	if (mode == "unsealed-batch")
		return writer.send(message_type::task_complete, control("task-1|complete")) ? EXIT_SUCCESS
																					: EXIT_FAILURE;
	if (!writer.send(message_type::column_chunk,
					 control(mode == "bad-column" ? "batch-1|unknown-column|0" : "batch-1|row|0"),
					 payload(row)))
		return EXIT_FAILURE;
	const auto sealed_digest = mode == "inconsistent-batch"
		? std::string{"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"}
		: rolling;
	if (!writer.send(message_type::batch_end, control("batch-1|1|" + sealed_digest)))
		return EXIT_FAILURE;
	const auto coverage =
		mode == "incomplete-coverage" ? "project|catalog|covered|" : "task|task-1|covered|";
	if (!writer.send(message_type::coverage_chunk, control(coverage)) ||
		!writer.send(message_type::unresolved_chunk, control("")) ||
		!writer.send(message_type::progress, control("")) ||
		!writer.send(message_type::task_complete, control("task-1|complete")))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
