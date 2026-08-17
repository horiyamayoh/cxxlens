#include "provider_task_v4_decode.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cxxlens::detail::clang22::source_closure
{
	namespace
	{
		[[nodiscard]] validation_error error(
			std::string code, std::string field = {}, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::uint64_t decode_u64(
			const std::span<const std::byte, 8U> bytes)
		{
			std::uint64_t result{};
			for (const auto byte : bytes)
				result = (result << 8U) | std::to_integer<unsigned char>(byte);
			return result;
		}

		class reader
		{
		  public:
			reader(task_v4_replay& replay, const task_v4_decode_limits limits)
				: replay_{replay}, limits_{limits}
			{
			}

			[[nodiscard]] std::expected<void, validation_error>
			exact(const std::span<std::byte> destination)
			{
				std::size_t copied{};
				while (copied < destination.size())
				{
					if (offset_ > replay_.size_bytes() ||
						destination.size() - copied > replay_.size_bytes() - offset_)
						return std::unexpected(error(
							"source-closure.task-v4-truncated", "stream",
							std::to_string(offset_)));
					auto read = replay_.read_at(offset_, destination.subspan(copied));
					if (!read)
						return std::unexpected(std::move(read.error()));
					if (*read == 0U || *read > destination.size() - copied)
						return std::unexpected(error(
							"source-closure.task-v4-short-read", "stream",
							std::to_string(offset_)));
					copied += *read;
					offset_ += *read;
				}
				return {};
			}

			[[nodiscard]] std::expected<std::uint64_t, validation_error>
			header(const std::byte expected_tag)
			{
				std::array<std::byte, 9U> value{};
				if (auto read = exact(value); !read)
					return std::unexpected(std::move(read.error()));
				if (value[0U] != expected_tag)
					return std::unexpected(error(
						"source-closure.task-v4-tag", "stream",
						std::to_string(
							std::to_integer<unsigned char>(value[0U]))));
				return decode_u64(
					std::span<const std::byte, 8U>{value.data() + 1U, 8U});
			}

			[[nodiscard]] std::expected<std::string, validation_error>
			string_frame(const std::byte tag, const std::size_t maximum)
			{
				auto size = header(tag);
				if (!size)
					return std::unexpected(std::move(size.error()));
				if (*size > maximum ||
					*size > std::numeric_limits<std::size_t>::max())
					return std::unexpected(error(
						"source-closure.task-v4-field-size", "stream",
						std::to_string(*size)));
				std::string result(static_cast<std::size_t>(*size), '\0');
				if (auto read =
						exact(std::as_writable_bytes(std::span{result})); !read)
					return std::unexpected(std::move(read.error()));
				return result;
			}

			[[nodiscard]] std::expected<std::uint64_t, validation_error>
			scalar(const std::byte tag)
			{
				auto size = header(tag);
				if (!size)
					return std::unexpected(std::move(size.error()));
				if (*size != 8U)
					return std::unexpected(error(
						"source-closure.task-v4-scalar-size", "stream",
						std::to_string(*size)));
				std::array<std::byte, 8U> value{};
				if (auto read = exact(value); !read)
					return std::unexpected(std::move(read.error()));
				return decode_u64(value);
			}

			[[nodiscard]] std::expected<std::vector<std::byte>, validation_error>
			blob_content(const std::byte tag, const std::uint64_t maximum)
			{
				auto size = header(tag);
				if (!size)
					return std::unexpected(std::move(size.error()));
				if (*size > maximum ||
					*size > std::numeric_limits<std::size_t>::max())
					return std::unexpected(error(
						"source-closure.task-v4-blob-size", "stream",
						std::to_string(*size)));
				std::vector<std::byte> result(
					static_cast<std::size_t>(*size));
				if (auto read = exact(result); !read)
					return std::unexpected(std::move(read.error()));
				return result;
			}

			[[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }
			[[nodiscard]] const task_v4_decode_limits& limits() const noexcept
			{
				return limits_;
			}

		  private:
			task_v4_replay& replay_;
			task_v4_decode_limits limits_;
			std::uint64_t offset_{};
		};

		[[nodiscard]] std::expected<file_role, validation_error>
		parse_role(const std::string& value)
		{
			if (value == "main-source") return file_role::main_source;
			if (value == "project-header") return file_role::project_header;
			if (value == "generated-header") return file_role::generated_header;
			if (value == "forced-include") return file_role::forced_include;
			if (value == "macro-file") return file_role::macro_file;
			return std::unexpected(error(
				"source-closure.task-v4-role", "role", value));
		}
	} // namespace

	std::expected<decoded_task_v4, validation_error>
	decode_task_v4(task_v4_replay& replay, const task_v4_decode_limits limits)
	{
		if (!replay.sealed())
			return std::unexpected(error(
				"source-closure.task-v4-unsealed", "stream"));
		if (limits.maximum_stream_bytes == 0U ||
			limits.maximum_string_bytes == 0U ||
			limits.maximum_arguments == 0U || limits.maximum_files == 0U ||
			limits.maximum_blobs == 0U ||
			replay.size_bytes() > limits.maximum_stream_bytes)
			return std::unexpected(error(
				"source-closure.task-v4-limit", "stream"));

		reader input{replay, limits};
		auto schema =
			input.string_frame(std::byte{0x01U}, limits.maximum_string_bytes);
		if (!schema || *schema != "cxxlens.clang22.task.v4")
			return std::unexpected(schema ? error(
				"source-closure.task-v4-schema", "schema", *schema) :
				std::move(schema.error()));
		auto task_id = input.string_frame(std::byte{0x02U}, 128U);
		if (!task_id) return std::unexpected(std::move(task_id.error()));
		auto main_path =
			input.string_frame(std::byte{0x03U}, limits.maximum_string_bytes);
		if (!main_path) return std::unexpected(std::move(main_path.error()));
		auto working_directory =
			input.string_frame(std::byte{0x04U}, limits.maximum_string_bytes);
		if (!working_directory)
			return std::unexpected(std::move(working_directory.error()));
		auto declared_closure_digest =
			input.string_frame(std::byte{0x05U}, 71U);
		if (!declared_closure_digest)
			return std::unexpected(std::move(declared_closure_digest.error()));

		auto argument_count = input.header(std::byte{0x06U});
		if (!argument_count)
			return std::unexpected(std::move(argument_count.error()));
		if (*argument_count < 2U ||
			*argument_count > limits.maximum_arguments)
			return std::unexpected(error(
				"source-closure.task-v4-argument-count", "arguments"));
		std::vector<std::string> arguments;
		arguments.reserve(static_cast<std::size_t>(*argument_count));
		for (std::uint64_t index = 0U; index < *argument_count; ++index)
		{
			auto argument = input.string_frame(
				std::byte{0x10U}, limits.maximum_string_bytes);
			if (!argument)
				return std::unexpected(std::move(argument.error()));
			arguments.push_back(std::move(*argument));
		}

		auto file_count = input.header(std::byte{0x20U});
		if (!file_count)
			return std::unexpected(std::move(file_count.error()));
		if (*file_count == 0U || *file_count > limits.maximum_files)
			return std::unexpected(error(
				"source-closure.task-v4-file-count", "files"));
		std::vector<file> files;
		files.reserve(static_cast<std::size_t>(*file_count));
		for (std::uint64_t index = 0U; index < *file_count; ++index)
		{
			auto path = input.string_frame(
				std::byte{0x21U}, limits.maximum_string_bytes);
			if (!path) return std::unexpected(std::move(path.error()));
			auto digest = input.string_frame(std::byte{0x22U}, 71U);
			if (!digest) return std::unexpected(std::move(digest.error()));
			auto size = input.scalar(std::byte{0x23U});
			if (!size) return std::unexpected(std::move(size.error()));
			auto role_text = input.string_frame(std::byte{0x24U}, 32U);
			if (!role_text)
				return std::unexpected(std::move(role_text.error()));
			auto role = parse_role(*role_text);
			if (!role) return std::unexpected(std::move(role.error()));
			auto provenance = input.string_frame(std::byte{0x25U}, 71U);
			if (!provenance)
				return std::unexpected(std::move(provenance.error()));
			files.push_back({std::move(*path), std::move(*digest), *size,
				*role, std::move(*provenance)});
		}

		auto blob_count = input.header(std::byte{0x30U});
		if (!blob_count)
			return std::unexpected(std::move(blob_count.error()));
		if (*blob_count == 0U || *blob_count > limits.maximum_blobs)
			return std::unexpected(error(
				"source-closure.task-v4-blob-count", "blobs"));
		std::vector<blob> blobs;
		blobs.reserve(static_cast<std::size_t>(*blob_count));
		for (std::uint64_t index = 0U; index < *blob_count; ++index)
		{
			auto digest = input.string_frame(std::byte{0x31U}, 71U);
			if (!digest) return std::unexpected(std::move(digest.error()));
			auto content = input.blob_content(
				std::byte{0x32U}, 16U * 1024U * 1024U);
			if (!content)
				return std::unexpected(std::move(content.error()));
			blobs.push_back({std::move(*digest), std::move(*content)});
		}
		if (input.offset() != replay.size_bytes())
			return std::unexpected(error(
				"source-closure.task-v4-trailing-bytes", "stream",
				std::to_string(replay.size_bytes() - input.offset())));

		snapshot closure_input;
		closure_input.files = std::move(files);
		closure_input.blobs = std::move(blobs);
		closure_input.path_case_policy = case_policy::portable_casefold;
		auto closure = validate(std::move(closure_input));
		if (!closure)
			return std::unexpected(std::move(closure.error()));
		if (closure->snapshot_digest != *declared_closure_digest)
			return std::unexpected(error(
				"source-closure.task-v4-closure-digest",
				"source_closure_digest", *declared_closure_digest));

		task_v4 task;
		task.task_id = std::move(*task_id);
		task.main_logical_path = std::move(*main_path);
		task.logical_working_directory = std::move(*working_directory);
		task.effective_arguments = std::move(arguments);
		task.closure =
			std::make_shared<const validated_snapshot>(std::move(*closure));
		auto derived = derive_task_v4_id(task);
		if (!derived || task.task_id != *derived)
			return std::unexpected(error(
				"source-closure.task-v4-id-mismatch", "task_id", task.task_id));
		return decoded_task_v4{std::move(task), input.offset()};
	}
} // namespace cxxlens::detail::clang22::source_closure
