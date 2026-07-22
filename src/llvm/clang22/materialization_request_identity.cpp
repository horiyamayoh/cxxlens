#include "materialization_request_identity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::string_view request_domain{"cxxlens.clang22-materialization-request.v2"};
		constexpr std::string_view semantic_request_domain{"cxxlens.clang22-semantic-request.v2"};
		constexpr std::string_view semantic_digest_marker{"cxxlens-semantic-digest-v2"};
		constexpr std::array identity_members{
			std::string_view{"materialization_request_id"},
			std::string_view{"request_digest"},
			std::string_view{"semantic_request_digest"},
		};
		constexpr std::array semantic_publication_exclusions{
			std::string_view{"backend"},
			std::string_view{"series_id"},
			std::string_view{"expected_parent_publication"},
			std::string_view{"sqlite_path"},
		};

		[[nodiscard]] bool exact_sha256(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		class production_identity_digest_factory final
			: public materialization_request_identity_digest_factory
		{
		  public:
			std::unique_ptr<materialization_digest_accumulator> make_digest() override
			{
				return make_materialization_sha256_accumulator();
			}
		};

		[[nodiscard]] sdk::error identity_error(std::string)
		{
			return materialization_admission_no_response();
		}

		[[nodiscard]] sdk::error identity_io_error(std::string detail,
												   const materialization_io_failure& failure)
		{
			return materialization_admission_io_failure(
				failure, "request-binding", "identity:" + std::move(detail));
		}

		[[nodiscard]] sdk::error mismatch(std::string field, std::string detail = {})
		{
			return {"materialization.identity-mismatch", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<std::uint64_t> checked_add(const std::uint64_t left,
															 const std::uint64_t right)
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return sdk::unexpected(materialization_admission_no_response());
			return left + right;
		}

		[[nodiscard]] bool contains(const std::span<const std::string_view> values,
									const std::string_view candidate)
		{
			return std::ranges::find(values, candidate) != values.end();
		}

		class canonical_writer
		{
		  public:
			explicit canonical_writer(materialization_digest_accumulator& digest) : digest_{digest}
			{
			}

			[[nodiscard]] sdk::result<void> write(const std::span<const std::byte> bytes)
			{
				if (auto updated = digest_.update(bytes); !updated)
					return sdk::unexpected(identity_io_error("digest-update", updated.error()));
				if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - emitted_bytes_)
					return sdk::unexpected(materialization_admission_no_response());
				emitted_bytes_ += static_cast<std::uint64_t>(bytes.size());
				return {};
			}

			[[nodiscard]] sdk::result<void> byte(const std::byte value)
			{
				return write(std::span{&value, 1U});
			}

			[[nodiscard]] sdk::result<void> length(const std::uint64_t value)
			{
				std::array<std::byte, 8U> encoded{};
				for (std::size_t index{}; index < encoded.size(); ++index)
					encoded[index] = static_cast<std::byte>(
						(value >> ((encoded.size() - 1U - index) * 8U)) & 0xffU);
				return write(encoded);
			}

			[[nodiscard]] std::uint64_t emitted_bytes() const noexcept
			{
				return emitted_bytes_;
			}

		  private:
			materialization_digest_accumulator& digest_;
			std::uint64_t emitted_bytes_{};
		};

		class raw_string_cursor
		{
		  public:
			raw_string_cursor(materialization_replayable_spool& spool,
							  const std::uint64_t begin,
							  const std::uint64_t end)
				: spool_{spool}, position_{begin}, end_{end}
			{
			}

			[[nodiscard]] sdk::result<int> get()
			{
				if (buffer_offset_ == buffer_size_)
				{
					if (position_ == end_)
						return -1;
					const auto remaining = end_ - position_;
					auto destination = std::span{buffer_}.first(static_cast<std::size_t>(
						std::min<std::uint64_t>(remaining, buffer_.size())));
					auto read = spool_.read_at(position_, destination);
					if (!read)
						return sdk::unexpected(identity_io_error(
							"source-content-read:byte=" + std::to_string(position_), read.error()));
					if (*read == 0U || *read > destination.size() || *read > end_ - position_)
						return sdk::unexpected(materialization_admission_no_response());
					buffer_offset_ = 0U;
					buffer_size_ = *read;
				}
				const auto value = std::to_integer<unsigned char>(buffer_[buffer_offset_++]);
				++position_;
				return static_cast<int>(value);
			}

			[[nodiscard]] std::uint64_t position() const noexcept
			{
				return position_;
			}

		  private:
			materialization_replayable_spool& spool_;
			std::array<std::byte, default_stream_chunk_bytes> buffer_{};
			std::size_t buffer_offset_{};
			std::size_t buffer_size_{};
			std::uint64_t position_{};
			std::uint64_t end_{};
		};

		class decoded_string_output
		{
		  public:
			explicit decoded_string_output(canonical_writer* output) : output_{output} {}

			[[nodiscard]] sdk::result<void> push(const unsigned char value)
			{
				if (decoded_size_ == std::numeric_limits<std::uint64_t>::max())
					return sdk::unexpected(materialization_admission_no_response());
				++decoded_size_;
				if (output_ == nullptr)
					return {};
				buffer_[buffer_size_++] = static_cast<std::byte>(value);
				if (buffer_size_ == buffer_.size())
					return flush();
				return {};
			}

			[[nodiscard]] sdk::result<void> finish()
			{
				return flush();
			}

			[[nodiscard]] std::uint64_t decoded_size() const noexcept
			{
				return decoded_size_;
			}

		  private:
			[[nodiscard]] sdk::result<void> flush()
			{
				if (output_ == nullptr || buffer_size_ == 0U)
					return {};
				if (auto written = output_->write(std::span{buffer_}.first(buffer_size_)); !written)
					return written;
				buffer_size_ = 0U;
				return {};
			}

			canonical_writer* output_{};
			std::array<std::byte, default_stream_chunk_bytes> buffer_{};
			std::size_t buffer_size_{};
			std::uint64_t decoded_size_{};
		};

		[[nodiscard]] sdk::result<std::uint32_t> hexadecimal_quad(raw_string_cursor& cursor)
		{
			std::uint32_t value{};
			for (std::size_t index{}; index < 4U; ++index)
			{
				auto next = cursor.get();
				if (!next || *next < 0)
					return sdk::unexpected(
						next ? identity_error("source-content-short-unicode-escape")
							 : std::move(next.error()));
				value <<= 4U;
				if (*next >= '0' && *next <= '9')
					value |= static_cast<std::uint32_t>(*next - '0');
				else if (*next >= 'a' && *next <= 'f')
					value |= static_cast<std::uint32_t>(*next - 'a' + 10);
				else if (*next >= 'A' && *next <= 'F')
					value |= static_cast<std::uint32_t>(*next - 'A' + 10);
				else
					return sdk::unexpected(identity_error("source-content-unicode-escape"));
			}
			return value;
		}

		[[nodiscard]] sdk::result<void> emit_code_point(decoded_string_output& output,
														const std::uint32_t code_point)
		{
			std::array<unsigned char, 4U> encoded{};
			std::size_t width{};
			if (code_point <= 0x7fU)
			{
				encoded[0U] = static_cast<unsigned char>(code_point);
				width = 1U;
			}
			else if (code_point <= 0x7ffU)
			{
				encoded[0U] = static_cast<unsigned char>(0xc0U | (code_point >> 6U));
				encoded[1U] = static_cast<unsigned char>(0x80U | (code_point & 0x3fU));
				width = 2U;
			}
			else if (code_point <= 0xffffU)
			{
				encoded[0U] = static_cast<unsigned char>(0xe0U | (code_point >> 12U));
				encoded[1U] = static_cast<unsigned char>(0x80U | ((code_point >> 6U) & 0x3fU));
				encoded[2U] = static_cast<unsigned char>(0x80U | (code_point & 0x3fU));
				width = 3U;
			}
			else
			{
				encoded[0U] = static_cast<unsigned char>(0xf0U | (code_point >> 18U));
				encoded[1U] = static_cast<unsigned char>(0x80U | ((code_point >> 12U) & 0x3fU));
				encoded[2U] = static_cast<unsigned char>(0x80U | ((code_point >> 6U) & 0x3fU));
				encoded[3U] = static_cast<unsigned char>(0x80U | (code_point & 0x3fU));
				width = 4U;
			}
			for (std::size_t index{}; index < width; ++index)
				if (auto pushed = output.push(encoded[index]); !pushed)
					return pushed;
			return {};
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		decode_json_string_token(materialization_replayable_spool& spool,
								 const std::uint64_t offset,
								 const std::uint64_t size,
								 canonical_writer* writer)
		{
			if (!spool.sealed() || size < 2U || offset > spool.size_bytes() ||
				size > spool.size_bytes() - offset)
				return sdk::unexpected(identity_error("source-content-span"));
			raw_string_cursor cursor{spool, offset, offset + size};
			auto opening = cursor.get();
			if (!opening || *opening != '"')
				return sdk::unexpected(opening ? identity_error("source-content-string-required")
											   : std::move(opening.error()));
			decoded_string_output output{writer};
			while (true)
			{
				auto next = cursor.get();
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				if (*next < 0)
					return sdk::unexpected(identity_error("source-content-unterminated"));
				const auto byte = static_cast<unsigned char>(*next);
				if (byte == '"')
				{
					if (cursor.position() != offset + size)
						return sdk::unexpected(identity_error("source-content-trailing-bytes"));
					if (auto finished = output.finish(); !finished)
						return sdk::unexpected(std::move(finished.error()));
					return output.decoded_size();
				}
				if (byte < 0x20U)
					return sdk::unexpected(identity_error("source-content-control-character"));
				if (byte == '\\')
				{
					auto escaped = cursor.get();
					if (!escaped || *escaped < 0)
						return sdk::unexpected(escaped
												   ? identity_error("source-content-short-escape")
												   : std::move(escaped.error()));
					switch (*escaped)
					{
						case '"':
						case '\\':
						case '/':
							if (auto pushed = output.push(static_cast<unsigned char>(*escaped));
								!pushed)
								return sdk::unexpected(std::move(pushed.error()));
							break;
						case 'b':
						case 'f':
						case 'n':
						case 'r':
						case 't':
						{
							constexpr std::array decoded{'\b', '\f', '\n', '\r', '\t'};
							constexpr std::string_view markers{"bfnrt"};
							const auto index = markers.find(static_cast<char>(*escaped));
							if (auto pushed =
									output.push(static_cast<unsigned char>(decoded[index]));
								!pushed)
								return sdk::unexpected(std::move(pushed.error()));
							break;
						}
						case 'u':
						{
							auto code_point = hexadecimal_quad(cursor);
							if (!code_point)
								return sdk::unexpected(std::move(code_point.error()));
							if (*code_point >= 0xd800U && *code_point <= 0xdbffU)
							{
								auto slash = cursor.get();
								auto marker = cursor.get();
								if (!slash || !marker)
									return sdk::unexpected(!slash ? std::move(slash.error())
																  : std::move(marker.error()));
								if (*slash != '\\' || *marker != 'u')
									return sdk::unexpected(
										identity_error("source-content-surrogate-pair"));
								auto low = hexadecimal_quad(cursor);
								if (!low || *low < 0xdc00U || *low > 0xdfffU)
									return sdk::unexpected(
										low ? identity_error("source-content-surrogate-pair")
											: std::move(low.error()));
								*code_point =
									0x10000U + ((*code_point - 0xd800U) << 10U) + (*low - 0xdc00U);
							}
							else if (*code_point >= 0xdc00U && *code_point <= 0xdfffU)
								return sdk::unexpected(
									identity_error("source-content-surrogate-pair"));
							if (auto emitted = emit_code_point(output, *code_point); !emitted)
								return sdk::unexpected(std::move(emitted.error()));
							break;
						}
						default:
							return sdk::unexpected(
								identity_error("source-content-unsupported-escape"));
					}
					continue;
				}

				std::size_t width{};
				std::uint32_t code_point{};
				std::uint32_t minimum{};
				if (byte < 0x80U)
				{
					if (auto pushed = output.push(byte); !pushed)
						return sdk::unexpected(std::move(pushed.error()));
					continue;
				}
				if (byte >= 0xc2U && byte <= 0xdfU)
				{
					width = 2U;
					code_point = byte & 0x1fU;
					minimum = 0x80U;
				}
				else if (byte >= 0xe0U && byte <= 0xefU)
				{
					width = 3U;
					code_point = byte & 0x0fU;
					minimum = 0x800U;
				}
				else if (byte >= 0xf0U && byte <= 0xf4U)
				{
					width = 4U;
					code_point = byte & 0x07U;
					minimum = 0x10000U;
				}
				else
					return sdk::unexpected(identity_error("source-content-invalid-utf8"));
				std::array<unsigned char, 4U> encoded{byte};
				for (std::size_t index = 1U; index < width; ++index)
				{
					auto continuation = cursor.get();
					if (!continuation || *continuation < 0 ||
						(static_cast<unsigned char>(*continuation) & 0xc0U) != 0x80U)
						return sdk::unexpected(continuation
												   ? identity_error("source-content-invalid-utf8")
												   : std::move(continuation.error()));
					encoded[index] = static_cast<unsigned char>(*continuation);
					code_point = (code_point << 6U) | (encoded[index] & 0x3fU);
				}
				if (code_point < minimum || code_point > 0x10ffffU ||
					(code_point >= 0xd800U && code_point <= 0xdfffU))
					return sdk::unexpected(identity_error("source-content-invalid-utf8"));
				for (std::size_t index{}; index < width; ++index)
					if (auto pushed = output.push(encoded[index]); !pushed)
						return sdk::unexpected(std::move(pushed.error()));
			}
		}

		[[nodiscard]] sdk::result<std::uint64_t> string_size(const std::string_view value)
		{
			return checked_add(9U, static_cast<std::uint64_t>(value.size()));
		}

		[[nodiscard]] sdk::result<void> emit_string(canonical_writer& output,
													const std::string_view value)
		{
			if (auto tag = output.byte(std::byte{0x04U}); !tag)
				return tag;
			if (auto length = output.length(value.size()); !length)
				return length;
			return output.write(std::as_bytes(std::span{value.data(), value.size()}));
		}

		[[nodiscard]] sdk::result<std::uint64_t> integer_size(const std::int64_t value)
		{
			const auto magnitude = value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
											 : static_cast<std::uint64_t>(value);
			std::uint64_t width{1U};
			for (auto remaining = magnitude; remaining > 0xffU; remaining >>= 8U)
				++width;
			return 10U + width;
		}

		[[nodiscard]] sdk::result<void> emit_integer(canonical_writer& output,
													 const std::int64_t value)
		{
			if (auto tag = output.byte(std::byte{0x02U}); !tag)
				return tag;
			if (auto sign = output.byte(value < 0 ? std::byte{0x01U} : std::byte{0x00U}); !sign)
				return sign;
			const auto magnitude = value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
											 : static_cast<std::uint64_t>(value);
			std::size_t width{1U};
			for (auto remaining = magnitude; remaining > 0xffU; remaining >>= 8U)
				++width;
			if (auto length = output.length(width); !length)
				return length;
			std::array<std::byte, 8U> encoded{};
			for (std::size_t index{}; index < width; ++index)
				encoded[index] =
					static_cast<std::byte>((magnitude >> ((width - 1U - index) * 8U)) & 0xffU);
			return output.write(std::span{encoded}.first(width));
		}

		struct source_string_binding
		{
			const json_value* placeholder{};
			std::uint64_t offset{};
			std::uint64_t size{};
		};

		class request_projection
		{
		  public:
			request_projection(materialization_replayable_spool& spool,
							   const materialization_request_envelope& envelope,
							   materialization_request_task_index& task_index,
							   const json_value& global_root)
				: spool_{spool}, envelope_{envelope}, task_index_{task_index},
				  global_root_{global_root}
			{
			}

			[[nodiscard]] sdk::result<std::uint64_t> size(const bool semantic)
			{
				const auto* object = global_root_.as_object();
				if (object == nullptr)
					return sdk::unexpected(identity_error("global-root-object"));
				std::uint64_t output{9U};
				for (const auto& [name, value] : *object)
				{
					if (contains(identity_members, name))
						continue;
					auto entry = root_entry_size(name, value, semantic);
					if (!entry)
						return sdk::unexpected(std::move(entry.error()));
					auto with_prefix = checked_add(8U, *entry);
					if (!with_prefix)
						return sdk::unexpected(std::move(with_prefix.error()));
					auto next = checked_add(output, *with_prefix);
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					output = *next;
				}
				return output;
			}

			[[nodiscard]] sdk::result<void> emit(canonical_writer& output, const bool semantic)
			{
				const auto* object = global_root_.as_object();
				if (object == nullptr)
					return sdk::unexpected(identity_error("global-root-object"));
				std::uint64_t count{};
				for (const auto& [name, value] : *object)
					if (!contains(identity_members, name))
						++count;
				if (auto header = emit_tuple_header(output, count); !header)
					return header;
				for (const auto& [name, value] : *object)
				{
					if (contains(identity_members, name))
						continue;
					auto entry = root_entry_size(name, value, semantic);
					if (!entry)
						return sdk::unexpected(std::move(entry.error()));
					if (auto length = output.length(*entry); !length)
						return length;
					if (auto header = emit_tuple_header(output, 2U); !header)
						return header;
					auto key_size = string_size(name);
					if (!key_size)
						return sdk::unexpected(std::move(key_size.error()));
					if (auto length = output.length(*key_size); !length)
						return length;
					if (auto key = emit_string(output, name); !key)
						return key;
					auto child_size = root_value_size(name, value, semantic);
					if (!child_size)
						return sdk::unexpected(std::move(child_size.error()));
					if (auto length = output.length(*child_size); !length)
						return length;
					if (auto child = emit_root_value(output, name, value, semantic); !child)
						return child;
				}
				return {};
			}

		  private:
			[[nodiscard]] sdk::result<void> emit_tuple_header(canonical_writer& output,
															  const std::uint64_t count)
			{
				if (auto tag = output.byte(std::byte{0x05U}); !tag)
					return tag;
				return output.length(count);
			}

			[[nodiscard]] sdk::result<std::uint64_t> root_entry_size(const std::string_view name,
																	 const json_value& value,
																	 const bool semantic)
			{
				auto key = string_size(name);
				auto child = root_value_size(name, value, semantic);
				if (!key)
					return sdk::unexpected(std::move(key.error()));
				if (!child)
					return sdk::unexpected(std::move(child.error()));
				auto size = checked_add(25U, *key);
				if (!size)
					return sdk::unexpected(std::move(size.error()));
				return checked_add(*size, *child);
			}

			[[nodiscard]] sdk::result<std::uint64_t> root_value_size(const std::string_view name,
																	 const json_value& value,
																	 const bool semantic)
			{
				if (name == "tasks")
					return task_array_size();
				if (semantic && name == "publication")
				{
					const auto* object = value.as_object();
					if (object == nullptr)
						return sdk::unexpected(identity_error("publication-object"));
					return object_size(*object, nullptr, semantic_publication_exclusions);
				}
				return value_size(value, nullptr);
			}

			[[nodiscard]] sdk::result<void> emit_root_value(canonical_writer& output,
															const std::string_view name,
															const json_value& value,
															const bool semantic)
			{
				if (name == "tasks")
					return emit_task_array(output);
				if (semantic && name == "publication")
				{
					const auto* object = value.as_object();
					if (object == nullptr)
						return sdk::unexpected(identity_error("publication-object"));
					return emit_object(output, *object, nullptr, semantic_publication_exclusions);
				}
				return emit_value(output, value, nullptr);
			}

			[[nodiscard]] sdk::result<materialization_request_task_span>
			bound_task(const std::uint64_t index)
			{
				if (!task_index_.sealed())
					return sdk::unexpected(identity_error("task-index-unsealed"));
				auto span = task_index_.at(index);
				if (!span)
					return sdk::unexpected(normalize_materialization_admission_spool_failure(
						std::move(span.error()), "request-binding", "task-index"));
				const auto tasks = std::ranges::find_if(envelope_.members(),
														[](const auto& member)
														{
															return member.name == "tasks";
														});
				if (tasks == envelope_.members().end() ||
					span->value_offset < tasks->value_offset ||
					span->value_offset > tasks->value_offset + tasks->value_size_bytes ||
					span->value_size_bytes >
						tasks->value_offset + tasks->value_size_bytes - span->value_offset)
					return sdk::unexpected(identity_error("task-index-envelope-binding"));
				return span;
			}

			[[nodiscard]] sdk::result<source_string_binding>
			source_binding(const json_document& task_document,
						   const materialization_request_task_span& task)
			{
				const auto* source = task_document.root().member("source");
				const auto* content =
					source != nullptr ? source->member("content_base64") : nullptr;
				if (source == nullptr || source->as_object() == nullptr || content == nullptr ||
					content->as_string() == nullptr || !content->as_string()->empty() ||
					!task.source_content_offset || !task.source_content_size_bytes)
					return sdk::unexpected(identity_error("source-content-binding"));
				return source_string_binding{content,
											 task.source_content_offset.value_or(0U),
											 task.source_content_size_bytes.value_or(0U)};
			}

			[[nodiscard]] sdk::result<std::uint64_t> task_array_size()
			{
				if (!task_index_.sealed() || task_index_.task_count() > 4096U)
					return sdk::unexpected(identity_error("task-index-binding"));
				std::uint64_t output{9U};
				std::uint64_t previous_end{};
				for (std::uint64_t index{}; index < task_index_.task_count(); ++index)
				{
					auto task = bound_task(index);
					if (!task)
						return sdk::unexpected(std::move(task.error()));
					if (index != 0U && task->value_offset <= previous_end)
						return sdk::unexpected(identity_error("task-index-order"));
					previous_end = task->value_offset + task->value_size_bytes;
					auto document = replay_materialization_task_metadata(
						spool_,
						*task,
						maximum_materialization_task_metadata_window_bytes,
						"request-binding");
					if (!document)
						return sdk::unexpected(std::move(document.error()));
					auto binding = source_binding(*document, *task);
					if (!binding)
						return sdk::unexpected(std::move(binding.error()));
					auto child = value_size(document->root(), &*binding);
					if (!child)
						return sdk::unexpected(std::move(child.error()));
					auto with_prefix = checked_add(8U, *child);
					if (!with_prefix)
						return sdk::unexpected(std::move(with_prefix.error()));
					auto next = checked_add(output, *with_prefix);
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					output = *next;
				}
				return output;
			}

			[[nodiscard]] sdk::result<void> emit_task_array(canonical_writer& output)
			{
				if (!task_index_.sealed() || task_index_.task_count() > 4096U)
					return sdk::unexpected(identity_error("task-index-binding"));
				if (auto header = emit_tuple_header(output, task_index_.task_count()); !header)
					return header;
				std::uint64_t previous_end{};
				for (std::uint64_t index{}; index < task_index_.task_count(); ++index)
				{
					auto task = bound_task(index);
					if (!task)
						return sdk::unexpected(std::move(task.error()));
					if (index != 0U && task->value_offset <= previous_end)
						return sdk::unexpected(identity_error("task-index-order"));
					previous_end = task->value_offset + task->value_size_bytes;
					auto document = replay_materialization_task_metadata(
						spool_,
						*task,
						maximum_materialization_task_metadata_window_bytes,
						"request-binding");
					if (!document)
						return sdk::unexpected(std::move(document.error()));
					auto binding = source_binding(*document, *task);
					if (!binding)
						return sdk::unexpected(std::move(binding.error()));
					auto child = value_size(document->root(), &*binding);
					if (!child)
						return sdk::unexpected(std::move(child.error()));
					if (auto length = output.length(*child); !length)
						return length;
					if (auto emitted = emit_value(output, document->root(), &*binding); !emitted)
						return emitted;
				}
				return {};
			}

			[[nodiscard]] sdk::result<std::uint64_t> value_size(const json_value& value,
																const source_string_binding* source)
			{
				if (source != nullptr && source->placeholder == &value)
				{
					auto decoded =
						decode_json_string_token(spool_, source->offset, source->size, nullptr);
					if (!decoded)
						return sdk::unexpected(std::move(decoded.error()));
					return checked_add(9U, *decoded);
				}
				switch (value.type())
				{
					case json_value::kind::null_value:
						return 1U;
					case json_value::kind::boolean:
						return 2U;
					case json_value::kind::signed_integer:
						return integer_size(*value.as_signed_integer());
					case json_value::kind::unsigned_integer:
					{
						const auto integer = *value.as_unsigned_integer();
						if (integer >
							static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
							return sdk::unexpected(materialization_admission_no_response());
						return integer_size(static_cast<std::int64_t>(integer));
					}
					case json_value::kind::string:
						return string_size(*value.as_string());
					case json_value::kind::array:
						return array_size(*value.as_array(), source);
					case json_value::kind::object:
						return object_size(*value.as_object(), source, {});
				}
				return sdk::unexpected(materialization_admission_no_response());
			}

			[[nodiscard]] sdk::result<std::uint64_t>
			array_size(const json_value::array_type& values, const source_string_binding* source)
			{
				std::uint64_t output{9U};
				for (const auto& value : values)
				{
					auto child = value_size(value, source);
					if (!child)
						return sdk::unexpected(std::move(child.error()));
					auto with_prefix = checked_add(8U, *child);
					if (!with_prefix)
						return sdk::unexpected(std::move(with_prefix.error()));
					auto next = checked_add(output, *with_prefix);
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					output = *next;
				}
				return output;
			}

			[[nodiscard]] sdk::result<std::uint64_t>
			object_size(const json_value::object_type& values,
						const source_string_binding* source,
						const std::span<const std::string_view> excluded)
			{
				std::uint64_t output{9U};
				for (const auto& [name, value] : values)
				{
					if (contains(excluded, name))
						continue;
					auto key = string_size(name);
					auto child = value_size(value, source);
					if (!key)
						return sdk::unexpected(std::move(key.error()));
					if (!child)
						return sdk::unexpected(std::move(child.error()));
					auto entry = checked_add(25U, *key);
					if (!entry)
						return sdk::unexpected(std::move(entry.error()));
					entry = checked_add(*entry, *child);
					if (!entry)
						return sdk::unexpected(std::move(entry.error()));
					auto with_prefix = checked_add(8U, *entry);
					if (!with_prefix)
						return sdk::unexpected(std::move(with_prefix.error()));
					auto next = checked_add(output, *with_prefix);
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					output = *next;
				}
				return output;
			}

			[[nodiscard]] sdk::result<void> emit_value(canonical_writer& output,
													   const json_value& value,
													   const source_string_binding* source)
			{
				if (source != nullptr && source->placeholder == &value)
				{
					auto size =
						decode_json_string_token(spool_, source->offset, source->size, nullptr);
					if (!size)
						return sdk::unexpected(std::move(size.error()));
					if (auto tag = output.byte(std::byte{0x04U}); !tag)
						return tag;
					if (auto length = output.length(*size); !length)
						return length;
					auto emitted =
						decode_json_string_token(spool_, source->offset, source->size, &output);
					if (!emitted)
						return sdk::unexpected(std::move(emitted.error()));
					if (*emitted != *size)
						return sdk::unexpected(identity_error("source-content-replay-size"));
					return {};
				}
				switch (value.type())
				{
					case json_value::kind::null_value:
						return output.byte(std::byte{0x00U});
					case json_value::kind::boolean:
						if (auto tag = output.byte(std::byte{0x01U}); !tag)
							return tag;
						return output.byte(*value.as_boolean() ? std::byte{0x01U}
															   : std::byte{0x00U});
					case json_value::kind::signed_integer:
						return emit_integer(output, *value.as_signed_integer());
					case json_value::kind::unsigned_integer:
					{
						const auto integer = *value.as_unsigned_integer();
						if (integer >
							static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
							return sdk::unexpected(materialization_admission_no_response());
						return emit_integer(output, static_cast<std::int64_t>(integer));
					}
					case json_value::kind::string:
						return emit_string(output, *value.as_string());
					case json_value::kind::array:
						return emit_array(output, *value.as_array(), source);
					case json_value::kind::object:
						return emit_object(output, *value.as_object(), source, {});
				}
				return sdk::unexpected(materialization_admission_no_response());
			}

			[[nodiscard]] sdk::result<void> emit_array(canonical_writer& output,
													   const json_value::array_type& values,
													   const source_string_binding* source)
			{
				if (auto header = emit_tuple_header(output, values.size()); !header)
					return header;
				for (const auto& value : values)
				{
					auto size = value_size(value, source);
					if (!size)
						return sdk::unexpected(std::move(size.error()));
					if (auto length = output.length(*size); !length)
						return length;
					if (auto emitted = emit_value(output, value, source); !emitted)
						return emitted;
				}
				return {};
			}

			[[nodiscard]] sdk::result<void>
			emit_object(canonical_writer& output,
						const json_value::object_type& values,
						const source_string_binding* source,
						const std::span<const std::string_view> excluded)
			{
				std::uint64_t count{};
				for (const auto& [name, value] : values)
					if (!contains(excluded, name))
						++count;
				if (auto header = emit_tuple_header(output, count); !header)
					return header;
				for (const auto& [name, value] : values)
				{
					if (contains(excluded, name))
						continue;
					auto key = string_size(name);
					auto child = value_size(value, source);
					if (!key)
						return sdk::unexpected(std::move(key.error()));
					if (!child)
						return sdk::unexpected(std::move(child.error()));
					auto entry = checked_add(25U, *key);
					if (!entry)
						return sdk::unexpected(std::move(entry.error()));
					entry = checked_add(*entry, *child);
					if (!entry)
						return sdk::unexpected(std::move(entry.error()));
					if (auto length = output.length(*entry); !length)
						return length;
					if (auto header = emit_tuple_header(output, 2U); !header)
						return header;
					if (auto length = output.length(*key); !length)
						return length;
					if (auto emitted = emit_string(output, name); !emitted)
						return emitted;
					if (auto length = output.length(*child); !length)
						return length;
					if (auto emitted = emit_value(output, value, source); !emitted)
						return emitted;
				}
				return {};
			}

			materialization_replayable_spool& spool_;
			const materialization_request_envelope& envelope_;
			materialization_request_task_index& task_index_;
			const json_value& global_root_;
		};

		[[nodiscard]] sdk::result<void> emit_tuple_header(canonical_writer& output,
														  const std::uint64_t count)
		{
			if (auto tag = output.byte(std::byte{0x05U}); !tag)
				return tag;
			return output.length(count);
		}

		[[nodiscard]] sdk::result<std::string>
		projection_digest(request_projection& projection,
						  const std::string_view domain,
						  const bool semantic,
						  materialization_request_identity_digest_factory& digest_factory)
		{
			auto projection_size = projection.size(semantic);
			if (!projection_size)
				return sdk::unexpected(std::move(projection_size.error()));
			auto accumulator = digest_factory.make_digest();
			if (!accumulator)
				return sdk::unexpected(identity_error("digest-create"));
			canonical_writer output{*accumulator};
			if (auto header = emit_tuple_header(output, 3U); !header)
				return sdk::unexpected(std::move(header.error()));

			for (const auto text : {semantic_digest_marker, domain})
			{
				auto encoded_size = string_size(text);
				if (!encoded_size)
					return sdk::unexpected(std::move(encoded_size.error()));
				if (auto length = output.length(*encoded_size); !length)
					return sdk::unexpected(std::move(length.error()));
				if (auto emitted = emit_string(output, text); !emitted)
					return sdk::unexpected(std::move(emitted.error()));
			}

			auto payload_size = checked_add(9U, *projection_size);
			if (!payload_size)
				return sdk::unexpected(std::move(payload_size.error()));
			if (auto length = output.length(*payload_size); !length)
				return sdk::unexpected(std::move(length.error()));
			if (auto tag = output.byte(std::byte{0x03U}); !tag)
				return sdk::unexpected(std::move(tag.error()));
			if (auto length = output.length(*projection_size); !length)
				return sdk::unexpected(std::move(length.error()));
			const auto projection_begin = output.emitted_bytes();
			if (auto emitted = projection.emit(output, semantic); !emitted)
				return sdk::unexpected(std::move(emitted.error()));
			if (output.emitted_bytes() - projection_begin != *projection_size)
				return sdk::unexpected(materialization_admission_no_response());

			auto digest = accumulator->finish();
			if (!digest)
				return sdk::unexpected(identity_io_error("digest-finish", digest.error()));
			if (!exact_sha256(*digest))
				return sdk::unexpected(materialization_admission_no_response());
			return "semantic-v2:" + *digest;
		}

		[[nodiscard]] const std::string* root_string(const json_value& root,
													 const std::string_view name)
		{
			const auto* value = root.member(name);
			return value != nullptr ? value->as_string() : nullptr;
		}
	} // namespace

	sdk::result<streamed_materialization_request_identity>
	derive_streamed_materialization_request_identity(
		materialization_replayable_spool& spool,
		const materialization_request_envelope& envelope,
		materialization_request_task_index& task_index)
	{
		production_identity_digest_factory digest_factory;
		return derive_streamed_materialization_request_identity(
			spool, envelope, task_index, digest_factory);
	}

	sdk::result<streamed_materialization_request_identity>
	derive_streamed_materialization_request_identity(
		materialization_replayable_spool& spool,
		const materialization_request_envelope& envelope,
		materialization_request_task_index& task_index,
		materialization_request_identity_digest_factory& digest_factory)
	{
		auto globals = replay_materialization_request_globals(
			spool,
			envelope,
			maximum_materialization_global_request_window_bytes,
			"request-binding");
		if (!globals)
			return sdk::unexpected(std::move(globals.error()));
		request_projection projection{spool, envelope, task_index, globals->root()};
		auto request = projection_digest(projection, request_domain, false, digest_factory);
		if (!request)
			return sdk::unexpected(std::move(request.error()));
		auto semantic =
			projection_digest(projection, semantic_request_domain, true, digest_factory);
		if (!semantic)
			return sdk::unexpected(std::move(semantic.error()));
		return streamed_materialization_request_identity{
			"materialization:" + *request, std::move(*request), std::move(*semantic)};
	}

	sdk::result<streamed_materialization_request_identity>
	validate_streamed_materialization_request_identity(
		materialization_replayable_spool& spool,
		const materialization_request_envelope& envelope,
		materialization_request_task_index& task_index)
	{
		production_identity_digest_factory digest_factory;
		return validate_streamed_materialization_request_identity(
			spool, envelope, task_index, digest_factory);
	}

	sdk::result<streamed_materialization_request_identity>
	validate_streamed_materialization_request_identity(
		materialization_replayable_spool& spool,
		const materialization_request_envelope& envelope,
		materialization_request_task_index& task_index,
		materialization_request_identity_digest_factory& digest_factory)
	{
		auto expected = derive_streamed_materialization_request_identity(
			spool, envelope, task_index, digest_factory);
		if (!expected)
			return sdk::unexpected(std::move(expected.error()));
		auto globals = replay_materialization_request_globals(
			spool,
			envelope,
			maximum_materialization_global_request_window_bytes,
			"request-binding");
		if (!globals)
			return sdk::unexpected(std::move(globals.error()));
		const auto* request = root_string(globals->root(), "request_digest");
		if (request == nullptr || *request != expected->request_digest)
			return sdk::unexpected(mismatch("request.request_digest", "exact-binding"));
		const auto* semantic = root_string(globals->root(), "semantic_request_digest");
		if (semantic == nullptr || *semantic != expected->semantic_request_digest)
			return sdk::unexpected(mismatch("request.semantic_request_digest", "exact-binding"));
		const auto* identifier = root_string(globals->root(), "materialization_request_id");
		if (identifier == nullptr || *identifier != expected->materialization_request_id)
			return sdk::unexpected(mismatch("request.materialization_request_id", "exact-binding"));
		return expected;
	}
} // namespace cxxlens::detail::clang22::materialization
