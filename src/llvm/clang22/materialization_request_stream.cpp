#include "materialization_request_stream.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include "materialization_json.hpp"
#include "materialization_task_spool.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error scan_error(std::string phase,
											std::string reason,
											const std::uint64_t offset,
											std::string code = "materialization.request-invalid")
		{
			return {std::move(code),
					std::move(phase),
					std::move(reason) + ":byte=" + std::to_string(offset)};
		}

		[[nodiscard]] sdk::error selected_shape_error(const std::string_view phase,
													  std::string reason,
													  const std::uint64_t offset)
		{
			if (phase == "request-schema")
				return scan_error("request-schema", std::move(reason), offset);
			return materialization_admission_no_response();
		}

		[[nodiscard]] sdk::error task_index_io_error(std::string phase,
													 std::string operation,
													 const std::uint64_t offset,
													 const materialization_io_failure& failure)
		{
			return materialization_admission_io_failure(failure,
														std::move(phase),
														"task-index:" + std::move(operation) +
															":byte=" + std::to_string(offset));
		}

		[[nodiscard]] sdk::error
		collision_metadata_io_error(const std::string_view operation,
									const materialization_io_failure& failure)
		{
			return materialization_admission_io_failure(
				failure, "request-schema", "task-collision-metadata:" + std::string{operation});
		}

		[[nodiscard]] sdk::error replay_io_error(const std::string_view phase,
												 const std::string_view operation,
												 const std::uint64_t offset,
												 const materialization_io_failure& failure)
		{
			return materialization_admission_io_failure(failure,
														std::string{phase},
														"raw-replay:" + std::string{operation} +
															":byte=" + std::to_string(offset));
		}

		class replay_cursor
		{
		  public:
			replay_cursor(materialization_replayable_spool& spool,
						  const std::size_t chunk_bytes,
						  const std::uint64_t begin = 0U,
						  const std::optional<std::uint64_t> end = std::nullopt,
						  const std::string_view phase = "json-decode")
				: spool_{spool}, buffer_(chunk_bytes), position_{begin},
				  end_{end.value_or(spool.size_bytes())}, phase_{phase}
			{
			}

			[[nodiscard]] sdk::result<int> peek()
			{
				if (buffer_offset_ == buffer_size_)
				{
					if (position_ == end_)
						return -1;
					if (position_ > end_)
						return sdk::unexpected(materialization_admission_no_response());
					const auto remaining = end_ - position_;
					auto destination = std::span{buffer_}.first(static_cast<std::size_t>(
						std::min<std::uint64_t>(remaining, buffer_.size())));
					auto read = spool_.read_at(position_, destination);
					if (!read)
						return sdk::unexpected(
							replay_io_error(phase_, "read", position_, read.error()));
					if (*read == 0U || *read > destination.size() || *read > end_ - position_)
						return sdk::unexpected(materialization_admission_no_response());
					buffer_offset_ = 0U;
					buffer_size_ = *read;
				}
				return static_cast<unsigned char>(buffer_[buffer_offset_]);
			}

			[[nodiscard]] sdk::result<int> get()
			{
				auto value = peek();
				if (!value || *value < 0)
					return value;
				++buffer_offset_;
				++position_;
				return value;
			}

			[[nodiscard]] std::uint64_t position() const noexcept
			{
				return position_;
			}

		  private:
			materialization_replayable_spool& spool_;
			std::vector<std::byte> buffer_;
			std::size_t buffer_offset_{};
			std::size_t buffer_size_{};
			std::uint64_t position_{};
			std::uint64_t end_{};
			std::string_view phase_;
		};

		void append_utf8(std::string* output,
						 const std::uint32_t code_point,
						 const std::size_t maximum_capture_bytes)
		{
			if (output == nullptr || output->size() >= maximum_capture_bytes)
				return;
			const auto begin = output->size();
			if (code_point <= 0x7fU)
				output->push_back(static_cast<char>(code_point));
			else if (code_point <= 0x7ffU)
			{
				output->push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
				output->push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
			}
			else if (code_point <= 0xffffU)
			{
				output->push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
				output->push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
				output->push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
			}
			else
			{
				output->push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
				output->push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
				output->push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
				output->push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
			}
			if (output->size() > maximum_capture_bytes)
				output->resize(std::max(begin, maximum_capture_bytes));
		}

		enum class task_capture_role : std::uint8_t
		{
			none,
			task,
			source,
		};

		struct task_span_capture
		{
			std::optional<materialization_request_envelope::member_span> source_content;
		};

		[[nodiscard]] sdk::result<unsigned char>
		replayed_byte(materialization_replayable_spool& spool,
					  const std::uint64_t offset,
					  const std::string_view phase)
		{
			std::byte value{};
			auto read = spool.read_at(offset, std::span{&value, 1U});
			if (!read)
				return sdk::unexpected(replay_io_error(phase, "read", offset, read.error()));
			if (*read != 1U)
				return sdk::unexpected(materialization_admission_no_response());
			return std::to_integer<unsigned char>(value);
		}

		/**
		 * Re-emit an already authenticated JSON token range in semantic compact form. This removes
		 * insignificant whitespace, decodes and minimally re-escapes strings, and reduces every
		 * accepted integer spelling to its canonical integer. Consequently the retained replay is
		 * bounded by selected-schema values rather than attacker-controlled raw spelling.
		 */
		class semantic_json_replayer
		{
		  public:
			semantic_json_replayer(materialization_replayable_spool& spool,
								   const std::uint64_t offset,
								   const std::uint64_t size,
								   std::string& output,
								   const std::size_t maximum_output_bytes,
								   const std::string_view phase)
				: cursor_{spool, default_stream_chunk_bytes, offset, offset + size, phase},
				  output_{output}, maximum_output_bytes_{maximum_output_bytes}
			{
			}

			[[nodiscard]] sdk::result<void> replay()
			{
				while (true)
				{
					auto next = cursor_.peek();
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					if (*next < 0)
						return {};
					const auto byte = static_cast<unsigned char>(*next);
					if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r')
					{
						if (auto consumed = cursor_.get(); !consumed)
							return sdk::unexpected(std::move(consumed.error()));
						continue;
					}
					if (byte == '"')
					{
						if (auto emitted = string(); !emitted)
							return emitted;
						continue;
					}
					if (byte == '-' || (byte >= '0' && byte <= '9'))
					{
						if (auto emitted = number(); !emitted)
							return emitted;
						continue;
					}
					if (byte == 't' || byte == 'f' || byte == 'n')
					{
						const auto literal = byte == 't' ? std::string_view{"true"}
							: byte == 'f'				 ? std::string_view{"false"}
														 : std::string_view{"null"};
						if (auto emitted = exact_literal(literal); !emitted)
							return emitted;
						continue;
					}
					if (byte != '{' && byte != '}' && byte != '[' && byte != ']' && byte != ':' &&
						byte != ',')
						return sdk::unexpected(materialization_admission_no_response());
					if (auto consumed = cursor_.get(); !consumed)
						return sdk::unexpected(std::move(consumed.error()));
					if (auto emitted = append(static_cast<char>(byte)); !emitted)
						return emitted;
				}
			}

		  private:
			struct decimal_state
			{
				bool all_zero{true};
				bool significant{};
				std::uint64_t coefficient_digits{};
				std::uint64_t pending_trailing_zeros{};
				std::array<char, 20U> prefix{};
				std::size_t prefix_size{};

				void observe(const char digit)
				{
					if (!significant)
					{
						if (digit == '0')
							return;
						significant = true;
						all_zero = false;
						coefficient_digits = 1U;
						prefix[prefix_size++] = digit;
						return;
					}
					if (digit == '0')
					{
						++pending_trailing_zeros;
						return;
					}
					all_zero = false;
					const auto retained = static_cast<std::size_t>(std::min<std::uint64_t>(
						pending_trailing_zeros, prefix.size() - prefix_size));
					std::fill_n(
						prefix.begin() + static_cast<std::ptrdiff_t>(prefix_size), retained, '0');
					prefix_size += retained;
					coefficient_digits += pending_trailing_zeros;
					pending_trailing_zeros = 0U;
					if (prefix_size < prefix.size())
						prefix[prefix_size++] = digit;
					++coefficient_digits;
				}
			};

			[[nodiscard]] sdk::result<void> append(const char byte)
			{
				if (output_.size() >= maximum_output_bytes_)
					return sdk::unexpected(materialization_admission_no_response());
				output_.push_back(byte);
				return {};
			}

			[[nodiscard]] sdk::result<void> append(const std::string_view bytes)
			{
				if (bytes.size() >
					maximum_output_bytes_ - std::min(maximum_output_bytes_, output_.size()))
					return sdk::unexpected(materialization_admission_no_response());
				output_.append(bytes);
				return {};
			}

			[[nodiscard]] sdk::result<void> exact_literal(const std::string_view literal)
			{
				for (const auto expected : literal)
				{
					auto consumed = cursor_.get();
					if (!consumed)
						return sdk::unexpected(std::move(consumed.error()));
					if (*consumed != static_cast<unsigned char>(expected))
						return sdk::unexpected(materialization_admission_no_response());
				}
				return append(literal);
			}

			[[nodiscard]] sdk::result<std::uint32_t> hexadecimal_quad()
			{
				std::uint32_t output{};
				for (std::size_t index{}; index < 4U; ++index)
				{
					auto digit = cursor_.get();
					if (!digit)
						return sdk::unexpected(std::move(digit.error()));
					if (*digit < 0)
						return sdk::unexpected(materialization_admission_no_response());
					output <<= 4U;
					const auto byte = static_cast<unsigned char>(*digit);
					if (byte >= '0' && byte <= '9')
						output |= byte - '0';
					else if (byte >= 'a' && byte <= 'f')
						output |= byte - 'a' + 10U;
					else if (byte >= 'A' && byte <= 'F')
						output |= byte - 'A' + 10U;
					else
						return sdk::unexpected(materialization_admission_no_response());
				}
				return output;
			}

			[[nodiscard]] sdk::result<void> append_code_point(const std::uint32_t code_point)
			{
				switch (code_point)
				{
					case '"':
						return append("\\\"");
					case '\\':
						return append("\\\\");
					case '\b':
						return append("\\b");
					case '\f':
						return append("\\f");
					case '\n':
						return append("\\n");
					case '\r':
						return append("\\r");
					case '\t':
						return append("\\t");
					default:
						break;
				}
				if (code_point < 0x20U)
				{
					constexpr std::string_view digits = "0123456789abcdef";
					std::array<char, 6U> escaped{'\\',
												 'u',
												 '0',
												 '0',
												 digits[(code_point >> 4U) & 0x0fU],
												 digits[code_point & 0x0fU]};
					return append(std::string_view{escaped.data(), escaped.size()});
				}
				if (code_point < 0x80U)
					return append(static_cast<char>(code_point));
				if (code_point > 0x10ffffU || (code_point >= 0xd800U && code_point <= 0xdfffU))
					return sdk::unexpected(materialization_admission_no_response());
				std::string encoded;
				encoded.reserve(4U);
				append_utf8(&encoded, code_point, 4U);
				return append(encoded);
			}

			[[nodiscard]] sdk::result<void> string()
			{
				auto opening = cursor_.get();
				if (!opening)
					return sdk::unexpected(std::move(opening.error()));
				if (*opening != '"')
					return sdk::unexpected(materialization_admission_no_response());
				if (auto emitted = append('"'); !emitted)
					return emitted;
				while (true)
				{
					auto next = cursor_.get();
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					if (*next < 0)
						return sdk::unexpected(materialization_admission_no_response());
					const auto byte = static_cast<unsigned char>(*next);
					if (byte == '"')
						return append('"');
					if (byte < 0x20U)
						return sdk::unexpected(materialization_admission_no_response());
					if (byte != '\\')
					{
						if (auto emitted = append(static_cast<char>(byte)); !emitted)
							return emitted;
						continue;
					}

					auto escaped = cursor_.get();
					if (!escaped)
						return sdk::unexpected(std::move(escaped.error()));
					if (*escaped < 0)
						return sdk::unexpected(materialization_admission_no_response());
					std::uint32_t code_point{};
					switch (static_cast<unsigned char>(*escaped))
					{
						case '"':
						case '\\':
						case '/':
							code_point = static_cast<unsigned char>(*escaped);
							break;
						case 'b':
							code_point = '\b';
							break;
						case 'f':
							code_point = '\f';
							break;
						case 'n':
							code_point = '\n';
							break;
						case 'r':
							code_point = '\r';
							break;
						case 't':
							code_point = '\t';
							break;
						case 'u':
						{
							auto decoded = hexadecimal_quad();
							if (!decoded)
								return sdk::unexpected(std::move(decoded.error()));
							code_point = *decoded;
							if (code_point >= 0xd800U && code_point <= 0xdbffU)
							{
								auto slash = cursor_.get();
								auto marker = cursor_.get();
								if (!slash || !marker)
									return sdk::unexpected(!slash ? std::move(slash.error())
																  : std::move(marker.error()));
								if (*slash != '\\' || *marker != 'u')
									return sdk::unexpected(materialization_admission_no_response());
								auto low = hexadecimal_quad();
								if (!low)
									return sdk::unexpected(std::move(low.error()));
								if (*low < 0xdc00U || *low > 0xdfffU)
									return sdk::unexpected(materialization_admission_no_response());
								code_point =
									0x10000U + ((code_point - 0xd800U) << 10U) + (*low - 0xdc00U);
							}
							else if (code_point >= 0xdc00U && code_point <= 0xdfffU)
								return sdk::unexpected(materialization_admission_no_response());
							break;
						}
						default:
							return sdk::unexpected(materialization_admission_no_response());
					}
					if (auto emitted = append_code_point(code_point); !emitted)
						return emitted;
				}
			}

			[[nodiscard]] sdk::result<bool> consume(const unsigned char expected)
			{
				auto next = cursor_.peek();
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				if (*next != expected)
					return false;
				auto consumed = cursor_.get();
				if (!consumed)
					return sdk::unexpected(std::move(consumed.error()));
				return true;
			}

			[[nodiscard]] sdk::result<void> number()
			{
				bool negative{};
				auto minus = consume('-');
				if (!minus)
					return sdk::unexpected(std::move(minus.error()));
				negative = *minus;
				auto first = cursor_.peek();
				if (!first)
					return sdk::unexpected(std::move(first.error()));
				if (*first < '0' || *first > '9')
					return sdk::unexpected(materialization_admission_no_response());

				decimal_state decimal;
				std::uint64_t integer_digits{};
				const bool leading_zero = *first == '0';
				while (true)
				{
					auto next = cursor_.peek();
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					if (*next < '0' || *next > '9')
						break;
					auto digit = cursor_.get();
					if (!digit)
						return sdk::unexpected(std::move(digit.error()));
					decimal.observe(static_cast<char>(*digit));
					++integer_digits;
				}
				if (leading_zero && integer_digits != 1U)
					return sdk::unexpected(materialization_admission_no_response());

				std::uint64_t fractional_digits{};
				auto point = consume('.');
				if (!point)
					return sdk::unexpected(std::move(point.error()));
				if (*point)
				{
					while (true)
					{
						auto next = cursor_.peek();
						if (!next)
							return sdk::unexpected(std::move(next.error()));
						if (*next < '0' || *next > '9')
							break;
						auto digit = cursor_.get();
						if (!digit)
							return sdk::unexpected(std::move(digit.error()));
						decimal.observe(static_cast<char>(*digit));
						++fractional_digits;
					}
					if (fractional_digits == 0U)
						return sdk::unexpected(materialization_admission_no_response());
				}

				bool exponent_negative{};
				bool exponent_overflow{};
				std::uint64_t exponent{};
				auto lower = consume('e');
				if (!lower)
					return sdk::unexpected(std::move(lower.error()));
				bool has_exponent = *lower;
				if (!has_exponent)
				{
					auto upper = consume('E');
					if (!upper)
						return sdk::unexpected(std::move(upper.error()));
					has_exponent = *upper;
				}
				if (has_exponent)
				{
					auto sign_minus = consume('-');
					if (!sign_minus)
						return sdk::unexpected(std::move(sign_minus.error()));
					exponent_negative = *sign_minus;
					if (!exponent_negative)
					{
						auto sign_plus = consume('+');
						if (!sign_plus)
							return sdk::unexpected(std::move(sign_plus.error()));
					}
					std::size_t exponent_digits{};
					while (true)
					{
						auto next = cursor_.peek();
						if (!next)
							return sdk::unexpected(std::move(next.error()));
						if (*next < '0' || *next > '9')
							break;
						auto digit = cursor_.get();
						if (!digit)
							return sdk::unexpected(std::move(digit.error()));
						++exponent_digits;
						const auto numeric = static_cast<std::uint64_t>(*digit - '0');
						if (exponent > (std::numeric_limits<std::uint64_t>::max() - numeric) / 10U)
							exponent_overflow = true;
						else if (!exponent_overflow)
							exponent = exponent * 10U + numeric;
					}
					if (exponent_digits == 0U)
						return sdk::unexpected(materialization_admission_no_response());
				}

				if (decimal.all_zero)
					return append('0');
				if (exponent_overflow)
					return sdk::unexpected(materialization_admission_no_response());

				bool scale_negative{};
				std::uint64_t scale{};
				const auto trailing = decimal.pending_trailing_zeros;
				if (exponent_negative)
				{
					if (exponent > std::numeric_limits<std::uint64_t>::max() - fractional_digits)
						scale_negative = true;
					else
					{
						const auto reduction = exponent + fractional_digits;
						if (trailing >= reduction)
							scale = trailing - reduction;
						else
						{
							scale_negative = true;
							scale = reduction - trailing;
						}
					}
				}
				else if (exponent > std::numeric_limits<std::uint64_t>::max() - trailing)
					scale = std::numeric_limits<std::uint64_t>::max();
				else
				{
					const auto increase = exponent + trailing;
					if (increase >= fractional_digits)
						scale = increase - fractional_digits;
					else
					{
						scale_negative = true;
						scale = fractional_digits - increase;
					}
				}
				if (scale_negative || decimal.coefficient_digits > 20U || scale > 20U ||
					decimal.coefficient_digits > 20U - scale)
					return sdk::unexpected(materialization_admission_no_response());

				std::string normalized{decimal.prefix.data(), decimal.prefix_size};
				normalized.append(static_cast<std::size_t>(scale), '0');
				const std::string_view bound =
					negative ? "9223372036854775808" : "18446744073709551615";
				if (normalized.size() > bound.size() ||
					(normalized.size() == bound.size() && normalized > bound))
					return sdk::unexpected(materialization_admission_no_response());
				if (negative)
					if (auto sign = append('-'); !sign)
						return sign;
				return append(normalized);
			}

			replay_cursor cursor_;
			std::string& output_;
			std::size_t maximum_output_bytes_{};
		};

		[[nodiscard]] sdk::result<void>
		append_semantic_replayed_range(materialization_replayable_spool& spool,
									   const std::uint64_t offset,
									   const std::uint64_t size,
									   std::string& output,
									   const std::size_t maximum_output_bytes,
									   const std::string_view phase)
		{
			return semantic_json_replayer{spool, offset, size, output, maximum_output_bytes, phase}
				.replay();
		}

		class indexed_json_string_reader
		{
		  public:
			indexed_json_string_reader(materialization_replayable_spool& spool,
									   const std::uint64_t offset,
									   const std::uint64_t size,
									   const std::string_view phase = "request-schema")
				: cursor_{spool, default_stream_chunk_bytes, offset, offset + size, phase},
				  phase_{phase}
			{
			}

			[[nodiscard]] sdk::result<void> open()
			{
				auto quote = cursor_.get();
				if (!quote)
					return sdk::unexpected(std::move(quote.error()));
				if (*quote != '"')
					return sdk::unexpected(invalid_source("source-content-string"));
				return {};
			}

			/** Return one decoded ASCII byte, or -1 at the exact closing quote. */
			[[nodiscard]] sdk::result<int> next()
			{
				if (ended_)
					return -1;
				auto value = cursor_.get();
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				if (*value < 0)
					return sdk::unexpected(invalid_source("source-content-unterminated"));
				const auto byte = static_cast<unsigned char>(*value);
				if (byte == '"')
				{
					auto end = cursor_.peek();
					if (!end)
						return sdk::unexpected(std::move(end.error()));
					if (*end != -1)
						return sdk::unexpected(invalid_source("source-content-span"));
					ended_ = true;
					return -1;
				}
				if (byte < 0x20U || byte >= 0x80U)
					return sdk::unexpected(invalid_source("source-content-ascii"));
				if (byte != '\\')
					return byte;

				auto escaped = cursor_.get();
				if (!escaped)
					return sdk::unexpected(std::move(escaped.error()));
				if (*escaped < 0)
					return sdk::unexpected(invalid_source("source-content-escape"));
				switch (static_cast<unsigned char>(*escaped))
				{
					case '"':
					case '\\':
					case '/':
						return *escaped;
					case 'b':
						return '\b';
					case 'f':
						return '\f';
					case 'n':
						return '\n';
					case 'r':
						return '\r';
					case 't':
						return '\t';
					case 'u':
					{
						std::uint32_t code_point{};
						for (std::size_t index{}; index < 4U; ++index)
						{
							auto digit = cursor_.get();
							if (!digit)
								return sdk::unexpected(std::move(digit.error()));
							if (*digit < 0)
								return sdk::unexpected(invalid_source("source-content-unicode"));
							code_point <<= 4U;
							const auto character = static_cast<unsigned char>(*digit);
							if (character >= '0' && character <= '9')
								code_point |= character - '0';
							else if (character >= 'a' && character <= 'f')
								code_point |= character - 'a' + 10U;
							else if (character >= 'A' && character <= 'F')
								code_point |= character - 'A' + 10U;
							else
								return sdk::unexpected(invalid_source("source-content-unicode"));
						}
						if (code_point >= 0x80U)
							return sdk::unexpected(invalid_source("source-content-ascii"));
						return static_cast<int>(code_point);
					}
					default:
						return sdk::unexpected(invalid_source("source-content-escape"));
				}
			}

		  private:
			[[nodiscard]] sdk::error invalid_source(const std::string_view reason) const
			{
				if (phase_ != "request-schema")
					return materialization_admission_no_response();
				return scan_error("request-schema", std::string{reason}, cursor_.position());
			}

			replay_cursor cursor_;
			std::string_view phase_;
			bool ended_{};
		};

		class request_scanner
		{
		  public:
			request_scanner(materialization_replayable_spool& spool,
							materialization_request_scan_limits limits,
							materialization_request_task_span_sink* task_index)
				: cursor_{spool, limits.chunk_bytes}, limits_{limits}, size_{spool.size_bytes()},
				  task_index_{task_index}
			{
			}

			[[nodiscard]] sdk::result<materialization_request_envelope> scan()
			{
				if (auto spaced = space(); !spaced)
					return sdk::unexpected(std::move(spaced.error()));
				auto first = cursor_.peek();
				if (!first)
					return sdk::unexpected(std::move(first.error()));
				if (*first != '{')
					return sdk::unexpected(
						scan_error("json-decode", "top-level-object-required", cursor_.position()));
				if (limits_.maximum_depth == 0U)
					return sdk::unexpected(
						scan_error("json-decode", "depth-limit", cursor_.position()));
				if (auto root = object(1U, true, nullptr, task_capture_role::none); !root)
					return sdk::unexpected(std::move(root.error()));
				if (auto spaced = space(); !spaced)
					return sdk::unexpected(std::move(spaced.error()));
				auto end = cursor_.peek();
				if (!end)
					return sdk::unexpected(std::move(end.error()));
				if (*end != -1 || cursor_.position() != size_)
					return sdk::unexpected(
						scan_error("json-decode", "trailing-data", cursor_.position()));
				if (!schema_ || !request_version_)
					return sdk::unexpected(
						scan_error("request-envelope", "missing-or-non-string-envelope", 0U));
				if (*schema_ != materialization_request_schema_v2)
					return sdk::unexpected(
						scan_error("request-envelope", "unsupported-schema", 0U));
				if (*request_version_ != materialization_request_version_v2_1)
					return sdk::unexpected(scan_error("request-version",
													  "unsupported-version",
													  0U,
													  "materialization.version-unsupported"));
				// Keep scanning the complete lexical document before reporting the selected
				// schema's top-level tasks maxItems contract. The compact task index intentionally
				// retains only the first 4096 spans and is never sealed on this failure path.
				if (tasks_exceeded_selected_maximum_)
					return sdk::unexpected(
						scan_error("request-schema", "tasks-maxItems", cursor_.position()));
				if (task_index_ != nullptr)
					if (auto sealed = task_index_->seal(); !sealed)
						return sdk::unexpected(std::move(sealed.error()));
				return materialization_request_envelope{std::move(*schema_),
														std::move(*request_version_),
														size_,
														std::move(top_level_members_)};
			}

		  private:
			[[nodiscard]] sdk::result<void> space()
			{
				while (true)
				{
					auto value = cursor_.peek();
					if (!value)
						return sdk::unexpected(std::move(value.error()));
					if (*value != ' ' && *value != '\t' && *value != '\n' && *value != '\r')
						return {};
					if (auto consumed = cursor_.get(); !consumed)
						return sdk::unexpected(std::move(consumed.error()));
				}
			}

			[[nodiscard]] sdk::result<bool> consume(const unsigned char expected)
			{
				auto value = cursor_.peek();
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				if (*value != expected)
					return false;
				auto consumed = cursor_.get();
				if (!consumed)
					return sdk::unexpected(std::move(consumed.error()));
				return true;
			}

			[[nodiscard]] sdk::result<void>
			value(const std::size_t depth,
				  std::string* captured_string = nullptr,
				  bool* captured_is_string = nullptr,
				  materialization_request_envelope::member_span* span = nullptr,
				  materialization_request_task_span_sink* array_index = nullptr,
				  task_span_capture* task_capture = nullptr,
				  task_capture_role capture_role = task_capture_role::none,
				  std::size_t maximum_capture_bytes = std::numeric_limits<std::size_t>::max())
			{
				if (captured_is_string != nullptr)
					*captured_is_string = false;
				if (auto spaced = space(); !spaced)
					return spaced;
				const auto begin = cursor_.position();
				auto parsed = value_body(depth,
										 captured_string,
										 captured_is_string,
										 array_index,
										 task_capture,
										 capture_role,
										 maximum_capture_bytes);
				if (parsed && span != nullptr)
				{
					span->value_offset = begin;
					span->value_size_bytes = cursor_.position() - begin;
				}
				return parsed;
			}

			[[nodiscard]] sdk::result<void>
			value_body(const std::size_t depth,
					   std::string* captured_string,
					   bool* captured_is_string,
					   materialization_request_task_span_sink* array_index,
					   task_span_capture* task_capture,
					   const task_capture_role capture_role,
					   const std::size_t maximum_capture_bytes)
			{
				auto next = cursor_.peek();
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				switch (*next)
				{
					case '{':
						if (depth >= limits_.maximum_depth)
							return sdk::unexpected(
								scan_error("json-decode", "depth-limit", cursor_.position()));
						return object(depth + 1U, false, task_capture, capture_role);
					case '[':
						if (depth >= limits_.maximum_depth)
							return sdk::unexpected(
								scan_error("json-decode", "depth-limit", cursor_.position()));
						return array(depth + 1U, array_index);
					case '"':
						if (captured_is_string != nullptr)
							*captured_is_string = true;
						return string(captured_string,
									  limits_.maximum_string_utf8_bytes,
									  maximum_capture_bytes);
					case 't':
						return literal("true");
					case 'f':
						return literal("false");
					case 'n':
						return literal("null");
					default:
						return number();
				}
			}

			[[nodiscard]] sdk::result<void> object(const std::size_t depth,
												   const bool top,
												   task_span_capture* task_capture,
												   const task_capture_role capture_role)
			{
				auto opening = consume('{');
				if (!opening || !*opening)
					return sdk::unexpected(
						opening ? scan_error("json-decode", "object-required", cursor_.position())
								: std::move(opening.error()));
				std::set<std::string, utf8_byte_less> names;
				if (auto spaced = space(); !spaced)
					return spaced;
				auto closed = consume('}');
				if (!closed)
					return sdk::unexpected(std::move(closed.error()));
				if (*closed)
					return {};

				while (true)
				{
					if (names.size() >= limits_.maximum_members_per_object)
						return sdk::unexpected(
							scan_error("json-decode", "object-member-limit", cursor_.position()));
					const auto key_offset = cursor_.position();
					std::string name;
					if (auto parsed = string(&name, limits_.maximum_member_name_utf8_bytes);
						!parsed)
						return parsed;
					if (!names.insert(name).second)
						return sdk::unexpected(
							scan_error("json-decode", "duplicate-member", key_offset));
					if (auto spaced = space(); !spaced)
						return spaced;
					auto colon = consume(':');
					if (!colon)
						return sdk::unexpected(std::move(colon.error()));
					if (!*colon)
						return sdk::unexpected(
							scan_error("json-decode", "object-colon", cursor_.position()));
					std::optional<std::string> envelope_value;
					const bool capture = top && (name == "schema" || name == "request_version");
					const auto capture_limit = name == "schema"
						? maximum_materialization_request_schema_capture_bytes
						: maximum_materialization_request_version_capture_bytes;
					std::string captured;
					bool captured_is_string{};
					materialization_request_envelope::member_span top_level_span;
					if (top)
						top_level_span.name = name;
					materialization_request_envelope::member_span source_content_span;
					const bool capture_source_content = task_capture != nullptr &&
						capture_role == task_capture_role::source && name == "content_base64";
					const auto child_role = task_capture != nullptr &&
							capture_role == task_capture_role::task && name == "source"
						? task_capture_role::source
						: task_capture_role::none;
					if (auto parsed = value(depth,
											capture ? &captured : nullptr,
											capture ? &captured_is_string : nullptr,
											top ? &top_level_span
												: capture_source_content ? &source_content_span
																		 : nullptr,
											top && name == "tasks" ? task_index_ : nullptr,
											task_capture,
											child_role,
											capture ? capture_limit
													: std::numeric_limits<std::size_t>::max());
						!parsed)
						return parsed;
					if (top)
						top_level_members_.push_back(std::move(top_level_span));
					if (capture_source_content)
						task_capture->source_content = std::move(source_content_span);
					if (capture && captured_is_string)
						envelope_value = std::move(captured);
					if (envelope_value)
					{
						if (name == "schema")
							schema_ = std::move(*envelope_value);
						else
							request_version_ = std::move(*envelope_value);
					}
					if (auto spaced = space(); !spaced)
						return spaced;
					closed = consume('}');
					if (!closed)
						return sdk::unexpected(std::move(closed.error()));
					if (*closed)
						return {};
					auto comma = consume(',');
					if (!comma)
						return sdk::unexpected(std::move(comma.error()));
					if (!*comma)
						return sdk::unexpected(
							scan_error("json-decode", "object-comma", cursor_.position()));
					if (auto spaced = space(); !spaced)
						return spaced;
				}
			}

			[[nodiscard]] sdk::result<void> array(const std::size_t depth,
												  materialization_request_task_span_sink* index)
			{
				auto opening = consume('[');
				if (!opening || !*opening)
					return sdk::unexpected(
						opening ? scan_error("json-decode", "array-required", cursor_.position())
								: std::move(opening.error()));
				if (auto spaced = space(); !spaced)
					return spaced;
				auto closed = consume(']');
				if (!closed)
					return sdk::unexpected(std::move(closed.error()));
				if (*closed)
					return {};
				std::size_t count{};
				while (true)
				{
					if (count >= limits_.maximum_elements_per_array)
						return sdk::unexpected(
							scan_error("json-decode", "array-element-limit", cursor_.position()));
					++count;
					materialization_request_envelope::member_span element;
					task_span_capture task_capture;
					if (auto parsed = value(depth,
											nullptr,
											nullptr,
											index != nullptr ? &element : nullptr,
											nullptr,
											index != nullptr ? &task_capture : nullptr,
											index != nullptr ? task_capture_role::task
															 : task_capture_role::none);
						!parsed)
						return parsed;
					if (index != nullptr && count <= 4096U)
					{
						const auto source_offset = task_capture.source_content
							? std::optional{task_capture.source_content->value_offset}
							: std::nullopt;
						const auto source_size = task_capture.source_content
							? std::optional{task_capture.source_content->value_size_bytes}
							: std::nullopt;
						if (auto appended = index->append(element.value_offset,
														  element.value_size_bytes,
														  source_offset,
														  source_size);
							!appended)
							return sdk::unexpected(std::move(appended.error()));
					}
					else if (index != nullptr)
						tasks_exceeded_selected_maximum_ = true;
					if (auto spaced = space(); !spaced)
						return spaced;
					closed = consume(']');
					if (!closed)
						return sdk::unexpected(std::move(closed.error()));
					if (*closed)
						return {};
					auto comma = consume(',');
					if (!comma)
						return sdk::unexpected(std::move(comma.error()));
					if (!*comma)
						return sdk::unexpected(
							scan_error("json-decode", "array-comma", cursor_.position()));
				}
			}

			[[nodiscard]] sdk::result<void> string(
				std::string* output,
				const std::size_t maximum_utf8_bytes,
				const std::size_t maximum_capture_bytes = std::numeric_limits<std::size_t>::max())
			{
				const auto begin = cursor_.position();
				auto quote = consume('"');
				if (!quote || !*quote)
					return sdk::unexpected(
						quote ? scan_error("json-decode", "string-required", cursor_.position())
							  : std::move(quote.error()));
				if (output != nullptr)
					output->clear();
				std::size_t decoded_bytes{};
				while (true)
				{
					auto next = cursor_.get();
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					if (*next < 0)
						return sdk::unexpected(
							scan_error("json-decode", "unterminated-string", begin));
					const auto byte = static_cast<unsigned char>(*next);
					if (byte == '"')
						return {};
					if (byte < 0x20U)
						return sdk::unexpected(scan_error(
							"json-decode", "raw-control-character", cursor_.position() - 1U));
					if (byte == '\\')
					{
						if (auto escaped = escape(
								output, decoded_bytes, maximum_utf8_bytes, maximum_capture_bytes);
							!escaped)
							return escaped;
						continue;
					}
					if (byte < 0x80U)
					{
						if (!append_byte(output,
										 static_cast<char>(byte),
										 decoded_bytes,
										 maximum_utf8_bytes,
										 maximum_capture_bytes))
							return sdk::unexpected(
								scan_error("json-decode", "string-byte-limit", begin));
						continue;
					}
					if (auto raw = raw_utf8(
							byte, output, decoded_bytes, maximum_utf8_bytes, maximum_capture_bytes);
						!raw)
						return raw;
				}
			}

			[[nodiscard]] bool append_byte(std::string* output,
										   const char byte,
										   std::size_t& decoded_bytes,
										   const std::size_t maximum,
										   const std::size_t maximum_capture_bytes) const
			{
				if (decoded_bytes >= maximum)
					return false;
				++decoded_bytes;
				if (output != nullptr && output->size() < maximum_capture_bytes)
					output->push_back(byte);
				return true;
			}

			[[nodiscard]] sdk::result<void> raw_utf8(const unsigned char first,
													 std::string* output,
													 std::size_t& decoded_bytes,
													 const std::size_t maximum,
													 const std::size_t maximum_capture_bytes)
			{
				std::size_t width{};
				std::uint32_t code_point{};
				std::uint32_t minimum{};
				if (first >= 0xc2U && first <= 0xdfU)
				{
					width = 2U;
					code_point = first & 0x1fU;
					minimum = 0x80U;
				}
				else if (first >= 0xe0U && first <= 0xefU)
				{
					width = 3U;
					code_point = first & 0x0fU;
					minimum = 0x800U;
				}
				else if (first >= 0xf0U && first <= 0xf4U)
				{
					width = 4U;
					code_point = first & 0x07U;
					minimum = 0x10000U;
				}
				else
					return sdk::unexpected(
						scan_error("json-decode", "invalid-utf8", cursor_.position() - 1U));
				std::array<unsigned char, 4U> bytes{first};
				for (std::size_t index = 1U; index < width; ++index)
				{
					auto next = cursor_.get();
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					if (*next < 0 || (static_cast<unsigned char>(*next) & 0xc0U) != 0x80U)
						return sdk::unexpected(
							scan_error("json-decode", "invalid-utf8", cursor_.position()));
					bytes[index] = static_cast<unsigned char>(*next);
					code_point = (code_point << 6U) | (bytes[index] & 0x3fU);
				}
				if (code_point < minimum || code_point > 0x10ffffU ||
					(code_point >= 0xd800U && code_point <= 0xdfffU) ||
					width > maximum - std::min(maximum, decoded_bytes))
					return sdk::unexpected(scan_error(
						"json-decode", "invalid-utf8-or-string-limit", cursor_.position()));
				decoded_bytes += width;
				if (output != nullptr)
					for (std::size_t index{}; index < width; ++index)
						if (output->size() < maximum_capture_bytes)
							output->push_back(static_cast<char>(bytes[index]));
				return {};
			}

			[[nodiscard]] sdk::result<std::uint32_t> hexadecimal_quad()
			{
				const auto begin = cursor_.position();
				std::uint32_t output{};
				for (std::size_t index{}; index < 4U; ++index)
				{
					auto next = cursor_.get();
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					if (*next < 0)
						return sdk::unexpected(
							scan_error("json-decode", "short-unicode-escape", begin));
					output <<= 4U;
					const auto byte = static_cast<unsigned char>(*next);
					if (byte >= '0' && byte <= '9')
						output |= byte - '0';
					else if (byte >= 'a' && byte <= 'f')
						output |= byte - 'a' + 10U;
					else if (byte >= 'A' && byte <= 'F')
						output |= byte - 'A' + 10U;
					else
						return sdk::unexpected(
							scan_error("json-decode", "unicode-escape", cursor_.position() - 1U));
				}
				return output;
			}

			[[nodiscard]] sdk::result<void> escape(std::string* output,
												   std::size_t& decoded_bytes,
												   const std::size_t maximum,
												   const std::size_t maximum_capture_bytes)
			{
				auto next = cursor_.get();
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				if (*next < 0)
					return sdk::unexpected(
						scan_error("json-decode", "short-escape", cursor_.position()));
				char decoded{};
				switch (static_cast<unsigned char>(*next))
				{
					case '"':
						decoded = '"';
						break;
					case '\\':
						decoded = '\\';
						break;
					case '/':
						decoded = '/';
						break;
					case 'b':
						decoded = '\b';
						break;
					case 'f':
						decoded = '\f';
						break;
					case 'n':
						decoded = '\n';
						break;
					case 'r':
						decoded = '\r';
						break;
					case 't':
						decoded = '\t';
						break;
					case 'u':
					{
						auto code_point = hexadecimal_quad();
						if (!code_point)
							return sdk::unexpected(std::move(code_point.error()));
						if (*code_point >= 0xd800U && *code_point <= 0xdbffU)
						{
							auto slash = cursor_.get();
							auto marker = cursor_.get();
							if (!slash || !marker)
								return sdk::unexpected(!slash ? std::move(slash.error())
															  : std::move(marker.error()));
							if (*slash != '\\' || *marker != 'u')
								return sdk::unexpected(scan_error(
									"json-decode", "surrogate-pair", cursor_.position()));
							auto low = hexadecimal_quad();
							if (!low || *low < 0xdc00U || *low > 0xdfffU)
								return sdk::unexpected(low ? scan_error("json-decode",
																		"surrogate-pair",
																		cursor_.position())
														   : std::move(low.error()));
							*code_point =
								0x10000U + ((*code_point - 0xd800U) << 10U) + (*low - 0xdc00U);
						}
						else if (*code_point >= 0xdc00U && *code_point <= 0xdfffU)
							return sdk::unexpected(
								scan_error("json-decode", "surrogate-pair", cursor_.position()));
						const auto width = *code_point <= 0x7fU ? 1U
							: *code_point <= 0x7ffU				? 2U
							: *code_point <= 0xffffU			? 3U
																: 4U;
						if (width > maximum - std::min(maximum, decoded_bytes))
							return sdk::unexpected(
								scan_error("json-decode", "string-byte-limit", cursor_.position()));
						decoded_bytes += width;
						append_utf8(output, *code_point, maximum_capture_bytes);
						return {};
					}
					default:
						return sdk::unexpected(scan_error(
							"json-decode", "unsupported-escape", cursor_.position() - 1U));
				}
				if (!append_byte(output, decoded, decoded_bytes, maximum, maximum_capture_bytes))
					return sdk::unexpected(
						scan_error("json-decode", "string-byte-limit", cursor_.position()));
				return {};
			}

			[[nodiscard]] sdk::result<void> literal(const std::string_view expected)
			{
				const auto begin = cursor_.position();
				for (const auto byte : expected)
				{
					auto next = cursor_.get();
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					if (*next != static_cast<unsigned char>(byte))
						return sdk::unexpected(scan_error("json-decode", "literal", begin));
				}
				return {};
			}

			struct decimal_state
			{
				bool all_zero{true};
				bool significant{};
				std::uint64_t coefficient_digits{};
				std::uint64_t pending_trailing_zeros{};
				std::array<char, 20U> prefix{};
				std::size_t prefix_size{};

				void observe(const char digit)
				{
					if (!significant)
					{
						if (digit == '0')
							return;
						significant = true;
						all_zero = false;
						coefficient_digits = 1U;
						prefix[prefix_size++] = digit;
						return;
					}
					if (digit == '0')
					{
						++pending_trailing_zeros;
						return;
					}
					all_zero = false;
					while (pending_trailing_zeros != 0U)
					{
						if (prefix_size < prefix.size())
							prefix[prefix_size++] = '0';
						++coefficient_digits;
						--pending_trailing_zeros;
					}
					if (prefix_size < prefix.size())
						prefix[prefix_size++] = digit;
					++coefficient_digits;
				}
			};

			[[nodiscard]] sdk::result<void> number()
			{
				const auto begin = cursor_.position();
				bool negative{};
				auto minus = consume('-');
				if (!minus)
					return sdk::unexpected(std::move(minus.error()));
				negative = *minus;
				auto first = cursor_.peek();
				if (!first)
					return sdk::unexpected(std::move(first.error()));
				if (*first < '0' || *first > '9')
					return sdk::unexpected(scan_error("json-decode", "number-syntax", begin));

				decimal_state decimal;
				std::uint64_t integer_digits{};
				const bool leading_zero = *first == '0';
				while (true)
				{
					auto next = cursor_.peek();
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					if (*next < '0' || *next > '9')
						break;
					auto digit = cursor_.get();
					if (!digit)
						return sdk::unexpected(std::move(digit.error()));
					decimal.observe(static_cast<char>(*digit));
					++integer_digits;
				}
				if (leading_zero && integer_digits != 1U)
					return sdk::unexpected(scan_error("json-decode", "number-syntax", begin));

				std::uint64_t fractional_digits{};
				auto point = consume('.');
				if (!point)
					return sdk::unexpected(std::move(point.error()));
				if (*point)
				{
					while (true)
					{
						auto next = cursor_.peek();
						if (!next)
							return sdk::unexpected(std::move(next.error()));
						if (*next < '0' || *next > '9')
							break;
						auto digit = cursor_.get();
						if (!digit)
							return sdk::unexpected(std::move(digit.error()));
						decimal.observe(static_cast<char>(*digit));
						++fractional_digits;
					}
					if (fractional_digits == 0U)
						return sdk::unexpected(scan_error("json-decode", "number-syntax", begin));
				}

				bool exponent_negative{};
				bool exponent_overflow{};
				std::uint64_t exponent{};
				auto exponent_marker_lower = consume('e');
				if (!exponent_marker_lower)
					return sdk::unexpected(std::move(exponent_marker_lower.error()));
				bool has_exponent = *exponent_marker_lower;
				if (!has_exponent)
				{
					auto exponent_marker_upper = consume('E');
					if (!exponent_marker_upper)
						return sdk::unexpected(std::move(exponent_marker_upper.error()));
					has_exponent = *exponent_marker_upper;
				}
				if (has_exponent)
				{
					auto sign_minus = consume('-');
					if (!sign_minus)
						return sdk::unexpected(std::move(sign_minus.error()));
					exponent_negative = *sign_minus;
					if (!exponent_negative)
					{
						auto sign_plus = consume('+');
						if (!sign_plus)
							return sdk::unexpected(std::move(sign_plus.error()));
					}
					std::size_t exponent_digits{};
					while (true)
					{
						auto next = cursor_.peek();
						if (!next)
							return sdk::unexpected(std::move(next.error()));
						if (*next < '0' || *next > '9')
							break;
						auto digit = cursor_.get();
						if (!digit)
							return sdk::unexpected(std::move(digit.error()));
						++exponent_digits;
						const auto numeric = static_cast<std::uint64_t>(*digit - '0');
						if (exponent > (std::numeric_limits<std::uint64_t>::max() - numeric) / 10U)
							exponent_overflow = true;
						else if (!exponent_overflow)
							exponent = exponent * 10U + numeric;
					}
					if (exponent_digits == 0U)
						return sdk::unexpected(scan_error("json-decode", "number-syntax", begin));
				}

				if (decimal.all_zero)
					return {};
				if (exponent_overflow)
					return sdk::unexpected(scan_error(
						"json-decode",
						exponent_negative ? "non-integer-number" : "unsigned-integer-overflow",
						begin));

				bool scale_negative{};
				std::uint64_t scale{};
				const auto trailing = decimal.pending_trailing_zeros;
				if (exponent_negative)
				{
					if (exponent > std::numeric_limits<std::uint64_t>::max() - fractional_digits)
						scale_negative = true;
					else
					{
						const auto reduction = exponent + fractional_digits;
						if (trailing >= reduction)
							scale = trailing - reduction;
						else
						{
							scale_negative = true;
							scale = reduction - trailing;
						}
					}
				}
				else if (exponent > std::numeric_limits<std::uint64_t>::max() - trailing)
					scale = std::numeric_limits<std::uint64_t>::max();
				else
				{
					const auto increase = exponent + trailing;
					if (increase >= fractional_digits)
						scale = increase - fractional_digits;
					else
					{
						scale_negative = true;
						scale = fractional_digits - increase;
					}
				}
				if (scale_negative)
					return sdk::unexpected(scan_error("json-decode", "non-integer-number", begin));
				if (decimal.coefficient_digits > 20U || scale > 20U ||
					decimal.coefficient_digits > 20U - scale)
					return sdk::unexpected(scan_error("json-decode",
													  negative ? "signed-integer-overflow"
															   : "unsigned-integer-overflow",
													  begin));

				std::string normalized{decimal.prefix.data(), decimal.prefix_size};
				normalized.append(static_cast<std::size_t>(scale), '0');
				const std::string_view bound =
					negative ? "9223372036854775808" : "18446744073709551615";
				if (normalized.size() > bound.size() ||
					(normalized.size() == bound.size() && normalized > bound))
					return sdk::unexpected(scan_error("json-decode",
													  negative ? "signed-integer-overflow"
															   : "unsigned-integer-overflow",
													  begin));
				return {};
			}

			replay_cursor cursor_;
			materialization_request_scan_limits limits_;
			std::uint64_t size_{};
			std::optional<std::string> schema_;
			std::optional<std::string> request_version_;
			std::vector<materialization_request_envelope::member_span> top_level_members_;
			materialization_request_task_span_sink* task_index_{};
			bool tasks_exceeded_selected_maximum_{};
		};
	} // namespace

	materialization_request_envelope::materialization_request_envelope(
		std::string schema,
		std::string request_version,
		const std::uint64_t scanned_size_bytes,
		std::vector<member_span> members)
		: schema_{std::move(schema)}, request_version_{std::move(request_version)},
		  scanned_size_bytes_{scanned_size_bytes}, members_{std::move(members)}
	{
	}

	const std::string& materialization_request_envelope::schema() const noexcept
	{
		return schema_;
	}

	const std::string& materialization_request_envelope::request_version() const noexcept
	{
		return request_version_;
	}

	std::uint64_t materialization_request_envelope::scanned_size_bytes() const noexcept
	{
		return scanned_size_bytes_;
	}

	const std::vector<materialization_request_envelope::member_span>&
	materialization_request_envelope::members() const noexcept
	{
		return members_;
	}

	materialization_request_task_index::materialization_request_task_index(
		std::unique_ptr<materialization_replayable_spool> spool,
		const std::uint64_t request_size_bytes)
		: spool_{std::move(spool)}, request_size_bytes_{request_size_bytes}
	{
	}

	std::uint64_t materialization_request_task_index::task_count() const noexcept
	{
		return task_count_;
	}

	bool materialization_request_task_index::sealed() const noexcept
	{
		return sealed_;
	}

	sdk::result<void> materialization_request_task_index::append(
		const std::uint64_t value_offset,
		const std::uint64_t value_size_bytes,
		const std::optional<std::uint64_t> source_content_offset,
		const std::optional<std::uint64_t> source_content_size_bytes)
	{
		if (sealed_ || value_size_bytes == 0U || value_offset > request_size_bytes_ ||
			value_size_bytes > request_size_bytes_ - value_offset || task_count_ >= 4096U ||
			source_content_offset.has_value() != source_content_size_bytes.has_value() ||
			(source_content_size_bytes &&
			 (*source_content_size_bytes == 0U || *source_content_offset < value_offset ||
			  *source_content_offset > value_offset + value_size_bytes ||
			  *source_content_size_bytes >
				  value_offset + value_size_bytes - *source_content_offset)))
			return sdk::unexpected(materialization_admission_no_response());
		std::array<std::byte, 32U> record{};
		const auto encode = [&](const std::uint64_t value, const std::size_t begin)
		{
			for (std::size_t index{}; index < 8U; ++index)
				record[begin + index] =
					static_cast<std::byte>((value >> ((7U - index) * 8U)) & 0xffU);
		};
		encode(value_offset, 0U);
		encode(value_size_bytes, 8U);
		encode(source_content_offset.value_or(0U), 16U);
		encode(source_content_size_bytes.value_or(0U), 24U);
		if (auto written = spool_->append(record); !written)
			return sdk::unexpected(
				task_index_io_error("json-decode", "append", value_offset, written.error()));
		++task_count_;
		return {};
	}

	sdk::result<void> materialization_request_task_index::seal()
	{
		if (sealed_)
			return sdk::unexpected(materialization_admission_no_response());
		if (auto sealed = spool_->seal(); !sealed)
			return sdk::unexpected(
				task_index_io_error("request-schema", "seal", 0U, sealed.error()));
		if (!spool_->sealed() || task_count_ > std::numeric_limits<std::uint64_t>::max() / 32U ||
			spool_->size_bytes() != task_count_ * 32U)
			return sdk::unexpected(materialization_admission_no_response());
		sealed_ = true;
		return {};
	}

	sdk::result<materialization_request_task_span>
	materialization_request_task_index::at(const std::uint64_t index)
	{
		if (!sealed_ || index >= task_count_ ||
			index > std::numeric_limits<std::uint64_t>::max() / 32U)
			return sdk::unexpected(materialization_admission_no_response());
		std::array<std::byte, 32U> record{};
		std::size_t filled{};
		const auto record_offset = index * 32U;
		while (filled != record.size())
		{
			auto read = spool_->read_at(record_offset + filled, std::span{record}.subspan(filled));
			if (!read)
				return sdk::unexpected(task_index_io_error(
					"task-index-private", "read", record_offset + filled, read.error()));
			if (*read == 0U || *read > record.size() - filled)
				return sdk::unexpected(materialization_admission_no_response());
			filled += *read;
		}
		const auto decode = [&](const std::size_t begin)
		{
			std::uint64_t value{};
			for (std::size_t offset{}; offset < 8U; ++offset)
				value = (value << 8U) | std::to_integer<unsigned char>(record[begin + offset]);
			return value;
		};
		const auto source_offset = decode(16U);
		const auto source_size = decode(24U);
		std::optional<std::uint64_t> decoded_source_offset;
		std::optional<std::uint64_t> decoded_source_size;
		if (source_size != 0U)
		{
			decoded_source_offset = source_offset;
			decoded_source_size = source_size;
		}
		materialization_request_task_span result{
			decode(0U), decode(8U), decoded_source_offset, decoded_source_size};
		if (result.value_size_bytes == 0U || result.value_offset > request_size_bytes_ ||
			result.value_size_bytes > request_size_bytes_ - result.value_offset ||
			(source_size == 0U && source_offset != 0U) ||
			(result.source_content_size_bytes &&
			 (*result.source_content_offset < result.value_offset ||
			  *result.source_content_offset > result.value_offset + result.value_size_bytes ||
			  *result.source_content_size_bytes >
				  result.value_offset + result.value_size_bytes - *result.source_content_offset)))
			return sdk::unexpected(materialization_admission_no_response());
		return result;
	}

	sdk::result<std::unique_ptr<materialization_request_task_index>>
	make_materialization_request_task_index(const std::uint64_t request_size_bytes)
	{
		if (request_size_bytes > maximum_raw_request_bytes)
			return sdk::unexpected(materialization_admission_no_response());
		auto spool = make_materialization_private_spool();
		if (!spool)
			return sdk::unexpected(task_index_io_error("json-decode", "create", 0U, spool.error()));
		return make_materialization_request_task_index(std::move(*spool), request_size_bytes);
	}

	sdk::result<std::unique_ptr<materialization_request_task_index>>
	make_materialization_request_task_index(
		std::unique_ptr<materialization_replayable_spool> storage,
		const std::uint64_t request_size_bytes)
	{
		if (request_size_bytes > maximum_raw_request_bytes)
			return sdk::unexpected(materialization_admission_no_response());
		if (!storage || storage->sealed() || storage->size_bytes() != 0U)
			return sdk::unexpected(materialization_admission_no_response());
		try
		{
			return std::unique_ptr<materialization_request_task_index>{
				new materialization_request_task_index{std::move(storage), request_size_bytes}};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}

	sdk::result<materialization_request_envelope>
	scan_materialization_request_envelope(materialization_replayable_spool& spool,
										  materialization_request_scan_limits limits,
										  materialization_request_task_span_sink* task_index)
	{
		constexpr materialization_request_scan_limits authority_limits{};
		if (!spool.sealed() || spool.size_bytes() > maximum_raw_request_bytes ||
			limits.chunk_bytes == 0U || limits.chunk_bytes > maximum_stream_chunk_bytes ||
			limits.maximum_depth != authority_limits.maximum_depth ||
			limits.maximum_members_per_object != authority_limits.maximum_members_per_object ||
			limits.maximum_elements_per_array != authority_limits.maximum_elements_per_array ||
			limits.maximum_member_name_utf8_bytes !=
				authority_limits.maximum_member_name_utf8_bytes ||
			limits.maximum_string_utf8_bytes != authority_limits.maximum_string_utf8_bytes)
			return sdk::unexpected(materialization_admission_no_response());
		try
		{
			return request_scanner{spool, limits, task_index}.scan();
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}

	sdk::result<json_document>
	replay_materialization_request_globals(materialization_replayable_spool& spool,
										   const materialization_request_envelope& envelope,
										   const std::size_t maximum_window_bytes,
										   const std::string_view phase)
	{
		if (phase != "request-schema" && phase != "request-binding")
			return sdk::unexpected(materialization_admission_no_response());
		if (maximum_window_bytes == 0U ||
			maximum_window_bytes > maximum_materialization_global_request_window_bytes)
			return sdk::unexpected(materialization_admission_no_response());
		try
		{
			constexpr std::array expected{
				std::string_view{"schema"},
				std::string_view{"request_version"},
				std::string_view{"materialization_request_id"},
				std::string_view{"request_digest"},
				std::string_view{"semantic_request_digest"},
				std::string_view{"tool"},
				std::string_view{"worker"},
				std::string_view{"project"},
				std::string_view{"registry"},
				std::string_view{"engine"},
				std::string_view{"interpretation_policy"},
				std::string_view{"trust_policy"},
				std::string_view{"group_topology"},
				std::string_view{"tasks"},
				std::string_view{"publication"},
			};
			if (!spool.sealed() || envelope.schema() != materialization_request_schema_v2 ||
				envelope.request_version() != materialization_request_version_v2_1 ||
				envelope.scanned_size_bytes() != spool.size_bytes())
				return sdk::unexpected(materialization_admission_no_response());

			std::set<std::string, utf8_byte_less> names;
			for (const auto& member : envelope.members())
				if (!names.insert(member.name).second)
					return sdk::unexpected(materialization_admission_no_response());
			if (names.size() != expected.size())
				return sdk::unexpected(selected_shape_error(phase, "root-member-census", 0U));
			for (const auto name : expected)
				if (!names.contains(name))
					return sdk::unexpected(selected_shape_error(phase, "root-member-required", 0U));

			for (const auto& member : envelope.members())
			{
				if (member.value_offset > spool.size_bytes() ||
					member.value_size_bytes > spool.size_bytes() - member.value_offset)
					return sdk::unexpected(materialization_admission_no_response());
			}

			std::string replay;
			replay.push_back('{');
			for (std::size_t member_index{}; member_index < envelope.members().size();
				 ++member_index)
			{
				const auto& member = envelope.members()[member_index];
				const auto fixed = member.name.size() + 3U + (member_index == 0U ? 0U : 1U);
				if (fixed > maximum_window_bytes - std::min(maximum_window_bytes, replay.size()))
					return sdk::unexpected(materialization_admission_no_response());
				if (member_index != 0U)
					replay.push_back(',');
				replay.push_back('"');
				replay.append(member.name);
				replay.append("\":");
				if (member.name == "tasks")
				{
					if (2U > maximum_window_bytes - std::min(maximum_window_bytes, replay.size()))
						return sdk::unexpected(materialization_admission_no_response());
					replay.append("[]");
					continue;
				}
				if (auto replayed = append_semantic_replayed_range(spool,
																   member.value_offset,
																   member.value_size_bytes,
																   replay,
																   maximum_window_bytes,
																   phase);
					!replayed)
					return sdk::unexpected(std::move(replayed.error()));
			}
			if (replay.size() >= maximum_window_bytes)
				return sdk::unexpected(materialization_admission_no_response());
			replay.push_back('}');
			json_limits limits;
			limits.max_input_bytes = maximum_window_bytes;
			limits.max_depth = 64U;
			// The raw/global window is already byte-bounded. Path-specific selected schemas,
			// including trust_policy.task_sandbox_requirements, own their exact maxItems contracts.
			limits.max_array_elements = maximum_window_bytes;
			limits.max_object_members = 4096U;
			limits.max_string_bytes = 8U * 1024U * 1024U;
			limits.max_total_string_bytes = maximum_window_bytes;
			limits.max_total_values = maximum_window_bytes;
			auto parsed = parse_json_object(std::move(replay), limits);
			if (!parsed)
				return sdk::unexpected(materialization_admission_no_response());
			return parsed;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}

	sdk::result<json_document>
	replay_materialization_task_metadata(materialization_replayable_spool& spool,
										 const materialization_request_task_span& task,
										 const std::size_t maximum_window_bytes,
										 const std::string_view phase)
	{
		if (phase != "request-schema" && phase != "request-binding")
			return sdk::unexpected(materialization_admission_no_response());
		if (maximum_window_bytes == 0U ||
			maximum_window_bytes > maximum_materialization_task_metadata_window_bytes)
			return sdk::unexpected(materialization_admission_no_response());
		try
		{
			if (!spool.sealed() || task.value_size_bytes == 0U ||
				task.value_offset > spool.size_bytes() ||
				task.value_size_bytes > spool.size_bytes() - task.value_offset ||
				task.source_content_offset.has_value() !=
					task.source_content_size_bytes.has_value())
				return sdk::unexpected(materialization_admission_no_response());
			if (!task.source_content_offset)
				return sdk::unexpected(
					selected_shape_error(phase, "source-content-required", task.value_offset));
			if (*task.source_content_size_bytes < 2U)
				return sdk::unexpected(selected_shape_error(
					phase, "source-content-string", *task.source_content_offset));
			if (*task.source_content_offset < task.value_offset ||
				*task.source_content_offset > task.value_offset + task.value_size_bytes ||
				*task.source_content_size_bytes >
					task.value_offset + task.value_size_bytes - *task.source_content_offset)
				return sdk::unexpected(materialization_admission_no_response());

			auto opening = replayed_byte(spool, *task.source_content_offset, phase);
			auto closing = replayed_byte(
				spool, *task.source_content_offset + *task.source_content_size_bytes - 1U, phase);
			if (!opening)
				return sdk::unexpected(std::move(opening.error()));
			if (!closing)
				return sdk::unexpected(std::move(closing.error()));
			if (*opening != '"' || *closing != '"')
				return sdk::unexpected(selected_shape_error(
					phase, "source-content-string", *task.source_content_offset));

			std::string replay;
			const auto prefix_size = *task.source_content_offset - task.value_offset;
			if (auto copied = append_semantic_replayed_range(
					spool, task.value_offset, prefix_size, replay, maximum_window_bytes, phase);
				!copied)
				return sdk::unexpected(std::move(copied.error()));
			if (2U > maximum_window_bytes - std::min(maximum_window_bytes, replay.size()))
				return sdk::unexpected(materialization_admission_no_response());
			replay.append("\"\"");
			const auto suffix_offset =
				*task.source_content_offset + *task.source_content_size_bytes;
			const auto task_end = task.value_offset + task.value_size_bytes;
			if (auto copied = append_semantic_replayed_range(spool,
															 suffix_offset,
															 task_end - suffix_offset,
															 replay,
															 maximum_window_bytes,
															 phase);
				!copied)
				return sdk::unexpected(std::move(copied.error()));

			json_limits limits;
			limits.max_input_bytes = maximum_window_bytes;
			limits.max_depth = 64U;
			limits.max_array_elements = 4096U;
			limits.max_object_members = 4096U;
			limits.max_string_bytes = 8U * 1024U * 1024U;
			limits.max_total_string_bytes = maximum_window_bytes;
			limits.max_total_values = 128U * 1024U;
			auto parsed = parse_json_object(std::move(replay), limits);
			if (!parsed)
				return sdk::unexpected(materialization_admission_no_response());
			return parsed;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}

	sdk::result<bool>
	exact_materialization_task_json_equal(materialization_replayable_spool& spool,
										  const materialization_request_task_span& left,
										  const materialization_request_task_span& right,
										  materialization_replayable_spool& metadata_comparison)
	{
		if (metadata_comparison.sealed() || metadata_comparison.size_bytes() != 0U)
			return sdk::unexpected(materialization_admission_no_response());
		std::string canonical_left;
		{
			auto left_metadata = replay_materialization_task_metadata(spool, left);
			if (!left_metadata)
				return sdk::unexpected(std::move(left_metadata.error()));
			canonical_left = canonical_json(left_metadata->root());
			if (auto appended = metadata_comparison.append(
					std::as_bytes(std::span{canonical_left.data(), canonical_left.size()}));
				!appended)
				return sdk::unexpected(collision_metadata_io_error("append", appended.error()));
		}
		if (auto sealed = metadata_comparison.seal(); !sealed)
			return sdk::unexpected(collision_metadata_io_error("seal", sealed.error()));
		if (!metadata_comparison.sealed() ||
			metadata_comparison.size_bytes() != canonical_left.size())
			return sdk::unexpected(materialization_admission_no_response());
		std::array<std::byte, default_stream_chunk_bytes> comparison{};
		std::uint64_t verified_left{};
		while (verified_left < canonical_left.size())
		{
			const auto destination = std::span{comparison}.first(static_cast<std::size_t>(
				std::min<std::uint64_t>(canonical_left.size() - verified_left, comparison.size())));
			auto read = metadata_comparison.read_at(verified_left, destination);
			if (!read)
				return sdk::unexpected(collision_metadata_io_error("read", read.error()));
			if (*read == 0U || *read > destination.size() ||
				*read > canonical_left.size() - verified_left)
				return sdk::unexpected(materialization_admission_no_response());
			const auto expected =
				std::as_bytes(std::span{canonical_left.data() + verified_left, *read});
			if (!std::ranges::equal(std::span{comparison}.first(*read), expected))
				return sdk::unexpected(materialization_admission_no_response());
			verified_left += *read;
		}

		std::string canonical_right;
		{
			auto right_metadata = replay_materialization_task_metadata(spool, right);
			if (!right_metadata)
				return sdk::unexpected(std::move(right_metadata.error()));
			canonical_right = canonical_json(right_metadata->root());
		}
		if (metadata_comparison.size_bytes() != canonical_right.size())
			return false;
		std::uint64_t compared{};
		while (compared < metadata_comparison.size_bytes())
		{
			const auto destination =
				std::span{comparison}.first(static_cast<std::size_t>(std::min<std::uint64_t>(
					metadata_comparison.size_bytes() - compared, comparison.size())));
			auto read = metadata_comparison.read_at(compared, destination);
			if (!read)
				return sdk::unexpected(collision_metadata_io_error("read", read.error()));
			if (*read == 0U || *read > destination.size())
				return sdk::unexpected(materialization_admission_no_response());
			const auto expected =
				std::as_bytes(std::span{canonical_right.data() + compared, *read});
			if (!std::ranges::equal(std::span{comparison}.first(*read), expected))
				return false;
			compared += *read;
		}

		if (left.source_content_offset.has_value() != right.source_content_offset.has_value() ||
			left.source_content_size_bytes.has_value() !=
				right.source_content_size_bytes.has_value() ||
			!left.source_content_offset || !left.source_content_size_bytes ||
			!right.source_content_offset || !right.source_content_size_bytes)
			return sdk::unexpected(
				scan_error("request-schema", "source-content-binding", left.value_offset));

		indexed_json_string_reader left_source{
			spool, *left.source_content_offset, *left.source_content_size_bytes};
		indexed_json_string_reader right_source{
			spool, *right.source_content_offset, *right.source_content_size_bytes};
		if (auto opened = left_source.open(); !opened)
			return sdk::unexpected(std::move(opened.error()));
		if (auto opened = right_source.open(); !opened)
			return sdk::unexpected(std::move(opened.error()));
		while (true)
		{
			auto left_byte = left_source.next();
			if (!left_byte)
				return sdk::unexpected(std::move(left_byte.error()));
			auto right_byte = right_source.next();
			if (!right_byte)
				return sdk::unexpected(std::move(right_byte.error()));
			if (*left_byte != *right_byte)
				return false;
			if (*left_byte < 0)
				return true;
		}
	}

	sdk::result<clang22_task_source_receipt>
	decode_materialization_task_source(materialization_replayable_spool& request,
									   const materialization_request_task_span& task,
									   clang22_task_source_spool& source,
									   const std::string_view phase)
	{
		if (phase != "request-schema" && phase != "request-binding")
			return sdk::unexpected(materialization_admission_no_response());
		const auto source_invalid = [&](const std::string_view reason) -> sdk::error
		{
			if (phase != "request-schema")
				return materialization_admission_no_response();
			return scan_error("request-schema", std::string{reason}, task.value_offset);
		};
		if (!request.sealed() ||
			task.source_content_offset.has_value() != task.source_content_size_bytes.has_value() ||
			!task.source_content_offset || *task.source_content_size_bytes < 2U ||
			*task.source_content_offset > request.size_bytes() ||
			*task.source_content_size_bytes > request.size_bytes() - *task.source_content_offset)
			return sdk::unexpected(source_invalid("source-content-binding"));

		indexed_json_string_reader input{
			request, *task.source_content_offset, *task.source_content_size_bytes, phase};
		if (auto opened = input.open(); !opened)
			return sdk::unexpected(std::move(opened.error()));
		const auto base64_value = [](const int character) -> std::optional<std::uint32_t>
		{
			if (character >= 'A' && character <= 'Z')
				return static_cast<std::uint32_t>(character - 'A');
			if (character >= 'a' && character <= 'z')
				return static_cast<std::uint32_t>(character - 'a' + 26);
			if (character >= '0' && character <= '9')
				return static_cast<std::uint32_t>(character - '0' + 52);
			if (character == '+')
				return 62U;
			if (character == '/')
				return 63U;
			return std::nullopt;
		};

		std::uint64_t encoded_count{};
		bool padded{};
		while (true)
		{
			std::array<int, 4U> characters{};
			std::size_t count{};
			for (; count < characters.size(); ++count)
			{
				auto next = input.next();
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				if (*next < 0)
					break;
				if (padded || encoded_count >= 22369624U)
					return sdk::unexpected(source_invalid("source-content-base64"));
				characters[count] = *next;
				++encoded_count;
			}
			if (count == 0U)
				break;
			if (count != characters.size())
				return sdk::unexpected(source_invalid("source-content-base64"));

			auto first = base64_value(characters[0U]);
			auto second = base64_value(characters[1U]);
			const bool padding_two = characters[2U] == '=';
			const bool padding_one = characters[3U] == '=';
			auto third =
				padding_two ? std::optional<std::uint32_t>{0U} : base64_value(characters[2U]);
			auto fourth =
				padding_one ? std::optional<std::uint32_t>{0U} : base64_value(characters[3U]);
			if (!first || !second || !third || !fourth || (padding_two && !padding_one) ||
				(padding_two && ((*second & 0x0fU) != 0U)) ||
				(padding_one && !padding_two && ((*third & 0x03U) != 0U)))
				return sdk::unexpected(source_invalid("source-content-base64"));
			std::array<std::byte, 3U> decoded{
				static_cast<std::byte>((*first << 2U) | (*second >> 4U)),
				static_cast<std::byte>((*second << 4U) | (*third >> 2U)),
				static_cast<std::byte>((*third << 6U) | *fourth),
			};
			const auto decoded_size = padding_two ? 1U : padding_one ? 2U : 3U;
			if (source.size_bytes() > maximum_clang22_task_source_bytes -
					std::min<std::uint64_t>(maximum_clang22_task_source_bytes, decoded_size))
				return sdk::unexpected(source_invalid("source-content-decoded-size"));
			if (auto appended = source.append(std::span{decoded}.first(decoded_size)); !appended)
				return sdk::unexpected(normalize_materialization_admission_spool_failure(
					std::move(appended.error()), phase, "source-spool:append"));
			padded = padding_one || padding_two;
		}
		auto sealed = source.seal();
		if (!sealed)
			return sdk::unexpected(normalize_materialization_admission_spool_failure(
				std::move(sealed.error()), phase, "source-spool:seal"));
		if (!source.sealed() || source.size_bytes() != sealed->size_bytes ||
			source.receipt() != *sealed)
			return sdk::unexpected(materialization_admission_no_response());
		if (sealed->size_bytes > maximum_clang22_task_source_bytes)
			return sdk::unexpected(source_invalid("source-content-decoded-size"));
		return sealed;
	}
} // namespace cxxlens::detail::clang22::materialization
