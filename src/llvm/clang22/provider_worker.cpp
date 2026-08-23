#include "provider_worker.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include <cxxlens/sdk/provider.hpp>

#include "sdk/provider_validation_internal.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using sdk::provider::message_type;

		constexpr std::string_view provider_id{"cxxlens.clang22.reference"};

		[[nodiscard]] bool canonical_digest(const std::string_view value) noexcept
		{
			const auto hex = value.starts_with("sha256:")  ? value.substr(7U)
				: value.starts_with("semantic-v2:sha256:") ? value.substr(19U)
														   : std::string_view{};
			return hex.size() == 64U &&
				std::ranges::all_of(hex,
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		class stream_source final : public sdk::provider::detail::host_input_byte_source
		{
		  public:
			explicit stream_source(std::istream& input) : input_{input} {}

			sdk::result<std::size_t> read(const std::span<std::byte> output) override
			{
				if (output.empty())
					return std::size_t{};
				input_.read(reinterpret_cast<char*>(output.data()),
							static_cast<std::streamsize>(output.size()));
				const auto count = input_.gcount();
				if (input_.bad() || (input_.fail() && !input_.eof()) || count < 0 ||
					static_cast<std::uintmax_t>(count) > output.size())
					return sdk::unexpected({"provider.input-read-failed", "stdin", "bounded-read"});
				return static_cast<std::size_t>(count);
			}

		  private:
			std::istream& input_;
		};

		/**
		 * The installed executable has no task-v3 decoder.  This sink lets the shared v2 host
		 * transcript validator consume and authenticate the bounded logical input without retaining
		 * source bytes in this fail-closed adapter.  The Protocol 2.0 dispatcher will replace this
		 * boundary with a task-v4/closure receiver when it is connected.
		 */
		class rejected_input_sink final : public sdk::provider::detail::host_input_chunk_sink
		{
		  public:
			sdk::result<void> append(const std::span<const std::byte> bytes) override
			{
				if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - observed_bytes_)
					return sdk::unexpected(
						{"provider.input-limit-exceeded", "payload", "byte-counter"});
				observed_bytes_ += static_cast<std::uint64_t>(bytes.size());
				return {};
			}

		  private:
			std::uint64_t observed_bytes_{};
		};

		class stream_sink final : public sdk::provider::frame_sink
		{
		  public:
			explicit stream_sink(std::ostream& output) : output_{&output} {}

			sdk::result<void> write(const std::span<const std::byte> data) override
			{
				output_->write(reinterpret_cast<const char*>(data.data()),
							   static_cast<std::streamsize>(data.size()));
				return output_->good() ? sdk::result<void>{}
									   : sdk::unexpected(sdk::error{
											 "provider.worker-write", "stdout", "write-failed"});
			}

		  private:
			std::ostream* output_;
		};

		[[nodiscard]] std::optional<std::string> environment(const char* name)
		{
			const auto* value = std::getenv(name);
			return value == nullptr ? std::nullopt : std::optional<std::string>{value};
		}

		[[nodiscard]] bool parse_version(const std::string_view text, std::uint16_t& output)
		{
			const auto [end, error] =
				std::from_chars(text.data(), text.data() + text.size(), output);
			return error == std::errc{} && end == text.data() + text.size();
		}
	} // namespace

	int run_provider_worker(std::istream& input, std::ostream& output)
	{
		const auto expected_manifest = environment("CXXLENS_PROVIDER_MANIFEST");
		const auto expected_provider_id = environment("CXXLENS_PROVIDER_ID");
		const auto expected_semantic_contract =
			environment("CXXLENS_PROVIDER_SEMANTIC_CONTRACT_DIGEST");
		const auto expected_task_id = environment("CXXLENS_PROVIDER_TASK_ID");
		const auto expected_task_digest = environment("CXXLENS_PROVIDER_TASK_INPUT_DIGEST");
		const auto expected_invocation =
			environment("CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST");
		const auto expected_toolchain = environment("CXXLENS_PROVIDER_TOOLCHAIN_DIGEST");
		const auto expected_environment = environment("CXXLENS_PROVIDER_ENVIRONMENT_DIGEST");
		const auto expected_major = environment("CXXLENS_PROVIDER_PROTOCOL_MAJOR");
		const auto expected_minor = environment("CXXLENS_PROVIDER_PROTOCOL_MINOR");
		if (!expected_manifest || !expected_provider_id || !expected_semantic_contract ||
			*expected_provider_id != provider_id ||
			!canonical_digest(*expected_semantic_contract) || !expected_task_id ||
			!expected_task_digest || !expected_invocation || !expected_toolchain ||
			!expected_environment || !expected_major || !expected_minor)
			return EXIT_FAILURE;

		sdk::provider::protocol_limits input_limits;
		if (!parse_version(*expected_major, input_limits.protocol_major) ||
			!parse_version(*expected_minor, input_limits.maximum_minor))
			return EXIT_FAILURE;
		input_limits.minimum_minor = input_limits.maximum_minor;

		stream_source source{input};
		rejected_input_sink input_sink;
		auto validated =
			sdk::provider::detail::validate_host_transcript_stream(source,
																   {{*expected_manifest,
																	 {*expected_task_id,
																	  *expected_task_digest,
																	  *expected_invocation,
																	  *expected_toolchain,
																	  *expected_environment},
																	 input_limits},
																	true},
																   input_sink);
		if (!validated)
			return EXIT_FAILURE;

		stream_sink sink{output};
		sdk::provider::protocol_writer writer{sink, input_limits};
		writer.grant_credit(validated->credit());
		auto hello = sdk::provider::encode_control_text(*expected_manifest);
		auto schema = sdk::provider::encode_schema_negotiate_metadata(
			{"cxxlens.provider-protocol.v2", input_limits.maximum_minor});
		if (!hello || !schema || !writer.send(message_type::hello, *hello) ||
			!writer.send(message_type::schema_negotiate, *schema))
			return EXIT_FAILURE;

		// A v3 payload is never decoded, interpreted, or passed to Clang.  Keeping the established
		// structured failure makes old callers fail closed while the protocol-v2 runtime adapter is
		// integrated; there is no silent fallback or dual-protocol acceptance.
		const auto control =
			sdk::provider::encode_task_failed_metadata({"provider.frontend-request-invalid",
														std::string{validated->task().task_id},
														"payload"});
		if (!control || !writer.send(message_type::task_failed, *control))
			return EXIT_FAILURE;
		return EXIT_SUCCESS;
	}
} // namespace cxxlens::detail::clang22
