#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "llvm/clang22/materialization_io.hpp"
#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/materialization_request_v2_2.hpp"

namespace
{
	using namespace cxxlens::detail::clang22::materialization;

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

	[[nodiscard]] bool validate_document(materialization_replayable_spool& spool)
	{
		constexpr std::size_t maximum_document_bytes =
			maximum_materialization_request_v2_2_document_bytes;
		if (!spool.sealed() || spool.size_bytes() == 0U ||
			spool.size_bytes() > maximum_document_bytes)
			return false;
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
				return false;
			raw.append(reinterpret_cast<const char*>(buffer.data()), *received);
			offset += static_cast<std::uint64_t>(*received);
		}
		const auto limits = materialization_request_v2_2_json_limits();
		auto document = parse_json_object(std::move(raw), limits);
		if (!document)
			return false;
		const auto* schema = document->root().member("schema");
		const auto* version = document->root().member("request_version");
		if (schema == nullptr || version == nullptr || schema->as_string() == nullptr ||
			version->as_string() == nullptr ||
			*schema->as_string() != materialization_request_v2_2_schema ||
			*version->as_string() != materialization_request_v2_2_version)
			return false;
		auto valid = validate_materialization_request_v2_2_document(document->root());
		if (!valid)
		{
			std::cerr << valid.error().code << '|' << valid.error().field << '|'
					  << valid.error().detail << '\n';
			return false;
		}
		return true;
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	const bool capture_only =
		argument_count == 2 && std::string_view{arguments[1]} == "--capture-only";
	if (argument_count != 1 && !capture_only)
		return 64;
	auto spool = make_materialization_private_spool();
	if (!spool)
		return 2;
	stdin_reader input;
	auto captured = capture_bounded_input(input, **spool);
	if (!captured)
		return 2;
	if (!captured->complete)
	{
		std::cout << "materialization.request-invalid|input-limit|maximum-bytes\n";
		return 1;
	}
	if (!capture_only && !validate_document(**spool))
	{
		std::cout << "materialization.request-invalid|request-schema|v2.2\n";
		return 1;
	}
	std::cout << "ok\n";
	return 0;
}
