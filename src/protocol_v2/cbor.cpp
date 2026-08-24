#include "cbor.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <ranges>
#include <set>

namespace cxxlens::protocol_v2::cbor
{
	namespace
	{
		enum class major : std::uint8_t
		{
			unsigned_integer = 0U,
			negative_integer = 1U,
			byte_string = 2U,
			text = 3U,
			array = 4U,
			map = 5U,
			simple = 7U,
		};

		[[nodiscard]] sdk::error failure(const std::string_view field,
										 const std::string_view detail)
		{
			return {"protocol-v2.cbor-invalid", std::string{field}, std::string{detail}};
		}

		void append_head(bytes& output, const major kind, const std::uint64_t argument)
		{
			const auto prefix = static_cast<std::uint8_t>(static_cast<std::uint8_t>(kind) << 5U);
			if (argument < 24U)
				output.push_back(static_cast<byte>(prefix | static_cast<std::uint8_t>(argument)));
			else if (argument <= std::numeric_limits<std::uint8_t>::max())
			{
				output.push_back(static_cast<byte>(prefix | 24U));
				output.push_back(static_cast<byte>(argument));
			}
			else if (argument <= std::numeric_limits<std::uint16_t>::max())
			{
				output.push_back(static_cast<byte>(prefix | 25U));
				output.push_back(static_cast<byte>(argument >> 8U));
				output.push_back(static_cast<byte>(argument));
			}
			else if (argument <= std::numeric_limits<std::uint32_t>::max())
			{
				output.push_back(static_cast<byte>(prefix | 26U));
				for (std::size_t shift = 24U; shift > 0U; shift -= 8U)
					output.push_back(static_cast<byte>(argument >> shift));
				output.push_back(static_cast<byte>(argument));
			}
			else
			{
				output.push_back(static_cast<byte>(prefix | 27U));
				for (std::size_t shift = 56U;; shift -= 8U)
				{
					output.push_back(static_cast<byte>(argument >> shift));
					if (shift == 0U)
						break;
				}
			}
		}

		[[nodiscard]] bytes encoded_text(const std::string_view text)
		{
			bytes output;
			output.reserve(9U + text.size());
			append_head(output, major::text, text.size());
			for (const auto character : text)
				output.push_back(static_cast<byte>(static_cast<unsigned char>(character)));
			return output;
		}

		struct encoder
		{
			bytes output;
			limits bound;
			std::size_t item_count{};

			[[nodiscard]] sdk::result<void> check_size() const
			{
				if (output.size() > bound.max_bytes)
					return sdk::unexpected(failure("bytes", "limit-exceeded"));
				return {};
			}

			[[nodiscard]] sdk::result<void> visit(const value& item, const std::size_t depth)
			{
				if (depth > bound.max_depth)
					return sdk::unexpected(failure("depth", "limit-exceeded"));
				if (item_count++ >= bound.max_items)
					return sdk::unexpected(failure("items", "limit-exceeded"));

				const auto append_bytes = [this](const std::span<const byte> bytes_to_append)
				{
					output.insert(output.end(), bytes_to_append.begin(), bytes_to_append.end());
				};

				if (std::holds_alternative<std::monostate>(item.data))
				{
					output.push_back(byte{0xf6});
				}
				else if (const auto* boolean = std::get_if<bool>(&item.data); boolean != nullptr)
				{
					output.push_back(*boolean ? byte{0xf5} : byte{0xf4});
				}
				else if (const auto* unsigned_integer = std::get_if<std::uint64_t>(&item.data);
						 unsigned_integer != nullptr)
				{
					append_head(output, major::unsigned_integer, *unsigned_integer);
				}
				else if (const auto* negative_integer = std::get_if<std::int64_t>(&item.data);
						 negative_integer != nullptr)
				{
					if (*negative_integer >= 0)
						return sdk::unexpected(failure("integer", "negative-kind-mismatch"));
					const auto magnitude =
						static_cast<std::uint64_t>(-(*negative_integer + 1)) + 1U;
					append_head(output, major::negative_integer, magnitude - 1U);
				}
				else if (const auto* byte_string = std::get_if<bytes>(&item.data);
						 byte_string != nullptr)
				{
					if (byte_string->size() > bound.max_byte_string_bytes)
						return sdk::unexpected(failure("bytes", "limit-exceeded"));
					append_head(output, major::byte_string, byte_string->size());
					append_bytes(*byte_string);
				}
				else if (const auto* text = std::get_if<std::string>(&item.data); text != nullptr)
				{
					if (text->size() > bound.max_text_bytes || !valid_utf8(*text))
						return sdk::unexpected(failure("text", "invalid-or-too-large"));
					append_head(output, major::text, text->size());
					for (const auto character : *text)
						output.push_back(static_cast<byte>(static_cast<unsigned char>(character)));
				}
				else if (const auto* items = std::get_if<array>(&item.data); items != nullptr)
				{
					if (items->size() > bound.max_items)
						return sdk::unexpected(failure("array", "limit-exceeded"));
					append_head(output, major::array, items->size());
					for (const auto& child : *items)
					{
						auto encoded = visit(child, depth + 1U);
						if (!encoded)
							return encoded;
					}
				}
				else
				{
					const auto* fields = std::get_if<map>(&item.data);
					if (fields == nullptr || fields->size() > bound.max_items)
						return sdk::unexpected(failure("map", "limit-exceeded"));

					struct entry
					{
						bytes key;
						const value* item{};
					};
					std::vector<entry> ordered;
					ordered.reserve(fields->size());
					std::set<std::string, std::less<>> unique_keys;
					for (const auto& field : *fields)
					{
						if (!unique_keys.insert(field.first).second)
							return sdk::unexpected(failure("map", "duplicate-key"));
						if (field.first.size() > bound.max_text_bytes || !valid_utf8(field.first))
							return sdk::unexpected(failure("map-key", "invalid-or-too-large"));
						ordered.push_back(entry{encoded_text(field.first), &field.second});
					}
					std::ranges::sort(ordered,
									  [](const entry& left, const entry& right)
									  {
										  if (left.key.size() != right.key.size())
											  return left.key.size() < right.key.size();
										  return std::lexicographical_compare(left.key.begin(),
																			  left.key.end(),
																			  right.key.begin(),
																			  right.key.end());
									  });
					append_head(output, major::map, ordered.size());
					for (const auto& field : ordered)
					{
						append_bytes(field.key);
						auto encoded = visit(*field.item, depth + 1U);
						if (!encoded)
							return encoded;
					}
				}
				return check_size();
			}
		};

		struct head
		{
			major kind{};
			std::uint64_t argument{};
			std::size_t next{};
		};

		[[nodiscard]] sdk::result<head> read_head(const std::span<const byte> input,
												  const std::size_t offset)
		{
			if (offset >= input.size())
				return sdk::unexpected(failure("cbor", "truncated-head"));
			const auto initial = std::to_integer<std::uint8_t>(input[offset]);
			const auto kind = static_cast<major>(initial >> 5U);
			const auto additional = initial & 0x1fU;
			if (additional >= 28U)
				return sdk::unexpected(failure("cbor", "indefinite-or-reserved"));
			if (additional < 24U)
				return head{kind, additional, offset + 1U};
			const auto width = additional == 24U ? 1U
				: additional == 25U				 ? 2U
				: additional == 26U				 ? 4U
												 : 8U;
			if (offset + 1U > input.size() || width > input.size() - offset - 1U)
				return sdk::unexpected(failure("cbor", "truncated-argument"));
			std::uint64_t argument{};
			for (std::size_t index{}; index < width; ++index)
				argument =
					(argument << 8U) | std::to_integer<std::uint64_t>(input[offset + 1U + index]);
			const auto shortest = width == 1U ? 24U
				: width == 2U				  ? 256U
				: width == 4U				  ? 65'536U
											  : (std::uint64_t{1U} << 32U);
			if (argument < shortest)
				return sdk::unexpected(failure("cbor", "non-shortest"));
			return head{kind, argument, offset + 1U + width};
		}

		struct decoder
		{
			std::span<const byte> input;
			limits bound;
			std::size_t item_count{};

			[[nodiscard]] sdk::result<value>
			visit(const std::size_t offset, const std::size_t depth, std::size_t& next)
			{
				if (depth > bound.max_depth)
					return sdk::unexpected(failure("depth", "limit-exceeded"));
				if (item_count++ >= bound.max_items)
					return sdk::unexpected(failure("items", "limit-exceeded"));
				auto parsed = read_head(input, offset);
				if (!parsed)
					return sdk::unexpected(parsed.error());
				next = parsed->next;
				const auto count = parsed->argument;
				switch (parsed->kind)
				{
					case major::unsigned_integer:
						return value{count};
					case major::negative_integer:
						if (count >
							static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
							return sdk::unexpected(failure("integer", "negative-overflow"));
						return value{static_cast<std::int64_t>(-1) -
									 static_cast<std::int64_t>(count)};
					case major::byte_string:
					{
						if (count > bound.max_byte_string_bytes || count > input.size() - next)
							return sdk::unexpected(failure("bytes", "limit-or-truncation"));
						bytes item(input.begin() + static_cast<std::ptrdiff_t>(next),
								   input.begin() + static_cast<std::ptrdiff_t>(next + count));
						next += static_cast<std::size_t>(count);
						return value{std::move(item)};
					}
					case major::text:
					{
						if (count > bound.max_text_bytes || count > input.size() - next)
							return sdk::unexpected(failure("text", "limit-or-truncation"));
						std::string item(reinterpret_cast<const char*>(input.data() + next),
										 static_cast<std::size_t>(count));
						if (!valid_utf8(item))
							return sdk::unexpected(failure("text", "invalid-utf8"));
						next += static_cast<std::size_t>(count);
						return value{std::move(item)};
					}
					case major::array:
					{
						if (count > bound.max_items)
							return sdk::unexpected(failure("array", "limit-exceeded"));
						array item;
						item.reserve(static_cast<std::size_t>(count));
						for (std::uint64_t index{}; index < count; ++index)
						{
							std::size_t child_next{};
							auto child = visit(next, depth + 1U, child_next);
							if (!child)
								return sdk::unexpected(child.error());
							item.push_back(std::move(*child));
							next = child_next;
						}
						return value{std::move(item)};
					}
					case major::map:
					{
						if (count > bound.max_items)
							return sdk::unexpected(failure("map", "limit-exceeded"));
						map item;
						item.reserve(static_cast<std::size_t>(count));
						bytes previous_key;
						for (std::uint64_t index{}; index < count; ++index)
						{
							const auto key_begin = next;
							std::size_t key_next{};
							auto key = visit(next, depth + 1U, key_next);
							if (!key || !std::holds_alternative<std::string>(key->data))
								return sdk::unexpected(failure("map-key", "text-required"));
							bytes encoded_key(
								input.begin() + static_cast<std::ptrdiff_t>(key_begin),
								input.begin() + static_cast<std::ptrdiff_t>(key_next));
							if (!previous_key.empty() &&
								!(previous_key.size() < encoded_key.size() ||
								  (previous_key.size() == encoded_key.size() &&
								   std::lexicographical_compare(previous_key.begin(),
																previous_key.end(),
																encoded_key.begin(),
																encoded_key.end()))))
								return sdk::unexpected(failure("map", "order-or-duplicate"));
							const auto& key_text = std::get<std::string>(key->data);
							if (find(item, key_text) != nullptr)
								return sdk::unexpected(failure("map", "duplicate-key"));
							previous_key = std::move(encoded_key);
							next = key_next;
							std::size_t value_next{};
							auto child = visit(next, depth + 1U, value_next);
							if (!child)
								return sdk::unexpected(child.error());
							item.emplace_back(key_text, std::move(*child));
							next = value_next;
						}
						return value{std::move(item)};
					}
					case major::simple:
						if (count == 20U)
							return value{false};
						if (count == 21U)
							return value{true};
						if (count == 22U)
							return value{nullptr};
						return sdk::unexpected(failure("simple", "unsupported"));
				}
				return sdk::unexpected(failure("cbor", "unsupported-major"));
			}
		};

		struct scanned_head
		{
			major kind{major::simple};
			std::uint64_t argument{};
			std::size_t next{};
			scan_error error{scan_error::none};
		};

		[[nodiscard]] scanned_head scan_head(const std::span<const byte> input,
											 const std::size_t offset) noexcept
		{
			if (offset >= input.size())
				return {.error = scan_error::truncated};
			const auto initial = std::to_integer<std::uint8_t>(input[offset]);
			const auto kind = static_cast<major>(initial >> 5U);
			const auto additional = initial & 0x1fU;
			if (additional >= 28U)
				return {.error = scan_error::indefinite_or_reserved};
			if (additional < 24U)
				return {kind, additional, offset + 1U, scan_error::none};
			const auto width = additional == 24U ? 1U
				: additional == 25U				 ? 2U
				: additional == 26U				 ? 4U
												 : 8U;
			if (width > input.size() - offset - 1U)
				return {.error = scan_error::truncated};
			std::uint64_t argument{};
			for (std::size_t index{}; index < width; ++index)
				argument =
					(argument << 8U) | std::to_integer<std::uint64_t>(input[offset + 1U + index]);
			const auto shortest = width == 1U ? 24U
				: width == 2U				  ? 256U
				: width == 4U				  ? 65'536U
											  : (std::uint64_t{1U} << 32U);
			if (argument < shortest)
				return {.error = scan_error::non_shortest};
			return {kind, argument, offset + 1U + width, scan_error::none};
		}

		[[nodiscard]] scan_result scan_canonical_impl(const std::span<const byte> input,
													  const scan_limits bound) noexcept
		{
			scan_workspace workspace;
			std::size_t cursor{};
			major root_kind{major::simple};
			bool root_complete{};
			while (!root_complete)
			{
				const auto depth = workspace.stack_size;
				if (depth > bound.max_depth || depth > maximum_scan_depth)
					return {scan_error::depth_limit, workspace.item_count, workspace.maximum_depth};
				if (workspace.item_count >= bound.max_items)
					return {scan_error::item_limit, workspace.item_count, workspace.maximum_depth};
				++workspace.item_count;
				workspace.maximum_depth = std::max(workspace.maximum_depth, depth);

				const auto item_begin = cursor;
				const auto parsed = scan_head(input, cursor);
				if (parsed.error != scan_error::none)
					return {parsed.error, workspace.item_count, workspace.maximum_depth};
				auto item_kind = parsed.kind;
				auto item_end = parsed.next;
				const auto count = parsed.argument;
				switch (item_kind)
				{
					case major::unsigned_integer:
						break;
					case major::negative_integer:
						if (count >
							static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
							return {scan_error::unsupported,
									workspace.item_count,
									workspace.maximum_depth};
						break;
					case major::byte_string:
						if (count > bound.max_byte_string_bytes || count > input.size() - item_end)
							return {scan_error::byte_string_limit,
									workspace.item_count,
									workspace.maximum_depth};
						item_end += static_cast<std::size_t>(count);
						break;
					case major::text:
					{
						if (count > bound.max_text_bytes || count > input.size() - item_end)
							return {scan_error::text_limit_or_utf8,
									workspace.item_count,
									workspace.maximum_depth};
						const auto size = static_cast<std::size_t>(count);
						const std::string_view text{
							reinterpret_cast<const char*>(input.data() + item_end), size};
						if (!valid_utf8(text))
							return {scan_error::text_limit_or_utf8,
									workspace.item_count,
									workspace.maximum_depth};
						item_end += size;
						break;
					}
					case major::array:
					case major::map:
					{
						const auto map_item = item_kind == major::map;
						const auto shape_limit =
							map_item ? bound.max_map_items : bound.max_array_items;
						if (count > shape_limit)
							return {map_item ? scan_error::map_shape_limit
											 : scan_error::array_shape_limit,
									workspace.item_count,
									workspace.maximum_depth};
						if (count > std::numeric_limits<std::size_t>::max())
							return {scan_error::item_limit,
									workspace.item_count,
									workspace.maximum_depth};
						const auto size = static_cast<std::size_t>(count);
						if (map_item && size > std::numeric_limits<std::size_t>::max() / 2U)
							return {scan_error::item_limit,
									workspace.item_count,
									workspace.maximum_depth};
						const auto children = map_item ? size * 2U : size;
						if (children > bound.max_items - workspace.item_count)
							return {scan_error::item_limit,
									workspace.item_count,
									workspace.maximum_depth};
						if (children != 0U)
						{
							if (workspace.stack_size >= workspace.stack.size())
								return {scan_error::depth_limit,
										workspace.item_count,
										workspace.maximum_depth};
							workspace.stack[workspace.stack_size++] =
								scan_stack_entry{children,
												 item_begin,
												 0U,
												 0U,
												 static_cast<std::uint8_t>(item_kind),
												 map_item};
							cursor = item_end;
							continue;
						}
						break;
					}
					case major::simple:
						if (count != 20U && count != 21U && count != 22U)
							return {scan_error::unsupported,
									workspace.item_count,
									workspace.maximum_depth};
						break;
					default:
						return {
							scan_error::unsupported, workspace.item_count, workspace.maximum_depth};
				}

				auto completed_begin = item_begin;
				for (;;)
				{
					cursor = item_end;
					if (workspace.stack_size == 0U)
					{
						root_kind = item_kind;
						root_complete = true;
						break;
					}
					auto& parent = workspace.stack[workspace.stack_size - 1U];
					const auto parent_kind = static_cast<major>(parent.container_kind);
					if (parent_kind == major::map && parent.expecting_key)
					{
						if (item_kind != major::text)
							return {scan_error::map_key_not_text,
									workspace.item_count,
									workspace.maximum_depth};
						const auto key_size = item_end - completed_begin;
						if (parent.previous_key_size != 0U)
						{
							const auto previous =
								input.subspan(parent.previous_key_offset, parent.previous_key_size);
							const auto current = input.subspan(completed_begin, key_size);
							if (!(previous.size() < current.size() ||
								  (previous.size() == current.size() &&
								   std::lexicographical_compare(previous.begin(),
																previous.end(),
																current.begin(),
																current.end()))))
								return {scan_error::map_order_or_duplicate,
										workspace.item_count,
										workspace.maximum_depth};
						}
						parent.previous_key_offset = completed_begin;
						parent.previous_key_size = key_size;
						parent.expecting_key = false;
					}
					else if (parent_kind == major::map)
						parent.expecting_key = true;
					--parent.remaining;
					if (parent.remaining != 0U)
						break;
					completed_begin = parent.item_begin;
					item_kind = parent_kind;
					--workspace.stack_size;
				}
			}

			if (bound.require_root_map && root_kind != major::map)
				return {scan_error::map_shape_limit, workspace.item_count, workspace.maximum_depth};
			if (cursor != input.size())
				return {scan_error::trailing_bytes, workspace.item_count, workspace.maximum_depth};
			return {scan_error::none, workspace.item_count, workspace.maximum_depth};
		}

		[[nodiscard]] sdk::result<void> validate_value(const value& item,
													   const limits bound,
													   const std::size_t depth,
													   std::size_t& count)
		{
			if (depth > bound.max_depth || count++ >= bound.max_items)
				return sdk::unexpected(failure("value", "limit-exceeded"));
			if (const auto* text = std::get_if<std::string>(&item.data); text != nullptr)
			{
				if (text->size() > bound.max_text_bytes || !valid_utf8(*text))
					return sdk::unexpected(failure("text", "invalid-or-too-large"));
			}
			else if (const auto* byte_string = std::get_if<bytes>(&item.data);
					 byte_string != nullptr)
			{
				if (byte_string->size() > bound.max_byte_string_bytes)
					return sdk::unexpected(failure("bytes", "limit-exceeded"));
			}
			else if (const auto* integer = std::get_if<std::int64_t>(&item.data);
					 integer != nullptr && *integer >= 0)
				return sdk::unexpected(failure("integer", "negative-kind-mismatch"));
			else if (const auto* items = std::get_if<array>(&item.data); items != nullptr)
			{
				if (items->size() > bound.max_items)
					return sdk::unexpected(failure("array", "limit-exceeded"));
				for (const auto& child : *items)
					if (auto valid = validate_value(child, bound, depth + 1U, count); !valid)
						return valid;
			}
			else if (const auto* fields = std::get_if<map>(&item.data); fields != nullptr)
			{
				if (fields->size() > bound.max_items)
					return sdk::unexpected(failure("map", "limit-exceeded"));
				std::set<std::string, std::less<>> keys;
				for (const auto& field : *fields)
				{
					if (!keys.insert(field.first).second)
						return sdk::unexpected(failure("map", "duplicate-key"));
					if (field.first.size() > bound.max_text_bytes || !valid_utf8(field.first))
						return sdk::unexpected(failure("map-key", "invalid-or-too-large"));
					if (auto valid = validate_value(field.second, bound, depth + 1U, count); !valid)
						return valid;
				}
			}
			return {};
		}
	} // namespace

	bool valid_utf8(const std::string_view text) noexcept
	{
		for (std::size_t index{}; index < text.size(); ++index)
		{
			const auto first = static_cast<std::uint8_t>(static_cast<unsigned char>(text[index]));
			if (first <= 0x7fU)
				continue;
			if (first >= 0xc2U && first <= 0xdfU)
			{
				if (index + 1U >= text.size())
					return false;
				const auto second =
					static_cast<std::uint8_t>(static_cast<unsigned char>(text[++index]));
				if (second < 0x80U || second > 0xbfU)
					return false;
				continue;
			}
			if (first >= 0xe0U && first <= 0xefU)
			{
				if (index + 2U >= text.size())
					return false;
				const auto second =
					static_cast<std::uint8_t>(static_cast<unsigned char>(text[++index]));
				const auto third =
					static_cast<std::uint8_t>(static_cast<unsigned char>(text[++index]));
				if (third < 0x80U || third > 0xbfU || second < 0x80U || second > 0xbfU)
					return false;
				if ((first == 0xe0U && second < 0xa0U) || (first == 0xedU && second > 0x9fU))
					return false;
				continue;
			}
			if (first >= 0xf0U && first <= 0xf4U)
			{
				if (index + 3U >= text.size())
					return false;
				const auto second =
					static_cast<std::uint8_t>(static_cast<unsigned char>(text[++index]));
				const auto third =
					static_cast<std::uint8_t>(static_cast<unsigned char>(text[++index]));
				const auto fourth =
					static_cast<std::uint8_t>(static_cast<unsigned char>(text[++index]));
				if (second < 0x80U || second > 0xbfU || third < 0x80U || third > 0xbfU ||
					fourth < 0x80U || fourth > 0xbfU)
					return false;
				if ((first == 0xf0U && second < 0x90U) || (first == 0xf4U && second > 0x8fU))
					return false;
				continue;
			}
			return false;
		}
		return true;
	}

	sdk::result<void> validate(const value& item, const limits bound)
	{
		std::size_t count{};
		return validate_value(item, bound, 0U, count);
	}

	sdk::result<bytes> encode(const value& item, const limits bound)
	{
		if (auto valid = validate(item, bound); !valid)
			return sdk::unexpected(valid.error());
		encoder writer{{}, bound, 0U};
		writer.output.reserve(std::min<std::size_t>(bound.max_bytes, 256U));
		if (auto result = writer.visit(item, 0U); !result)
			return sdk::unexpected(result.error());
		return std::move(writer.output);
	}

	sdk::result<value> decode(const std::span<const byte> input, const limits bound)
	{
		if (input.empty() || input.size() > bound.max_bytes)
			return sdk::unexpected(failure("bytes", "empty-or-limit-exceeded"));
		decoder reader{input, bound, 0U};
		std::size_t next{};
		auto output = reader.visit(0U, 0U, next);
		if (!output)
			return sdk::unexpected(output.error());
		if (next != input.size())
			return sdk::unexpected(failure("cbor", "trailing-bytes"));
		return std::move(*output);
	}

	scan_result scan_canonical(const std::span<const byte> input, const scan_limits bound) noexcept
	{
		if (input.empty() || input.size() > bound.max_bytes || bound.max_items == 0U)
			return {scan_error::empty_or_limit, 0U, 0U};
		return scan_canonical_impl(input, bound);
	}

	const value* find(const map& fields, const std::string_view key) noexcept
	{
		const auto item = std::ranges::find(fields, key, &std::pair<std::string, value>::first);
		return item == fields.end() ? nullptr : &item->second;
	}

	sdk::result<void> require_keys(const map& fields,
								   const std::initializer_list<std::string_view> keys)
	{
		if (fields.size() != keys.size())
			return sdk::unexpected(failure("map", "field-count"));
		for (const auto key : keys)
		{
			if (find(fields, key) == nullptr)
				return sdk::unexpected(failure("map", "missing-or-unknown-field"));
		}
		return {};
	}
} // namespace cxxlens::protocol_v2::cbor
