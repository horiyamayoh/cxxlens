#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>

#include "llvm/clang22/materialization_io.hpp"
#include "llvm/clang22/materialization_request_stream.hpp"
#include "llvm/clang22/materialization_request_v2_1.hpp"

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

	int fail(const cxxlens::sdk::error& error)
	{
		std::cout << error.code << '|' << error.field << '|' << error.detail << '\n';
		return 1;
	}

	int admission_failure(const cxxlens::sdk::error& error)
	{
		return is_materialization_admission_no_response(error) ? 2 : fail(error);
	}
} // namespace

int main(const int argc, char** argv)
{
	using namespace cxxlens::detail::clang22::materialization;
	if (argc == 2 && std::string_view{argv[1]} == "--test-no-response")
		return admission_failure(materialization_admission_no_response());
	const bool test_input_limit = argc == 2 && std::string_view{argv[1]} == "--test-input-limit";
	if (argc != 1 && !test_input_limit)
		return 2;
	auto spool = make_materialization_private_spool();
	if (!spool)
		return 2;
	stdin_reader input;
	bounded_input_options options;
	if (test_input_limit)
		options.byte_limit = 16U;
	auto observed = capture_bounded_input(input, **spool, options);
	if (!observed)
		return 2;
	if (!observed->complete)
		return fail({"materialization.request-invalid", "input-limit", "maximum-bytes"});

	auto index = make_materialization_request_task_index((*spool)->size_bytes());
	if (!index)
		return admission_failure(index.error());
	auto envelope = scan_materialization_request_envelope(**spool, {}, index->get());
	if (!envelope)
		return admission_failure(envelope.error());
	auto request = validate_materialization_request_v2_1(
		std::move(*spool), std::move(*envelope), std::move(*index));
	if (!request)
	{
		return admission_failure(request.error());
	}
	std::cout << "ok\n";
}
