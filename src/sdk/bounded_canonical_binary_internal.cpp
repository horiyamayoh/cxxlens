#include "bounded_canonical_binary_internal.hpp"

#include <utility>

namespace cxxlens::sdk::detail
{
	namespace
	{
		class decoder
		{
		  public:
			decoder(const std::size_t maximum_nesting_depth,
					std::string_view invalid_error_code,
					std::string_view limit_error_code)
				: maximum_nesting_depth_{maximum_nesting_depth},
				  invalid_error_code_{invalid_error_code}, limit_error_code_{limit_error_code}
			{
			}

			[[nodiscard]] result<void> preflight(const std::span<const std::byte> input,
												 const std::size_t depth) const
			{
				if (depth > maximum_nesting_depth_)
					return unexpected(limit("nesting-depth"));
				if (input.empty())
					return unexpected(invalid("missing-tag"));
				std::size_t offset{1U};
				switch (std::to_integer<unsigned char>(input.front()))
				{
					case 0x00U:
						break;
					case 0x01U:
						if (input.size() - offset != 1U)
							return unexpected(invalid("boolean-size"));
						++offset;
						break;
					case 0x02U:
					{
						if (offset == input.size())
							return unexpected(invalid("integer-sign"));
						++offset;
						auto width = read_length(input, offset);
						if (!width)
							return unexpected(std::move(width.error()));
						if (*width > input.size() - offset)
							return unexpected(invalid("integer-width"));
						offset += static_cast<std::size_t>(*width);
						break;
					}
					case 0x03U:
					case 0x04U:
					{
						auto size = read_length(input, offset);
						if (!size)
							return unexpected(std::move(size.error()));
						if (*size > input.size() - offset)
							return unexpected(invalid("payload-size"));
						offset += static_cast<std::size_t>(*size);
						break;
					}
					case 0x05U:
					{
						auto count = read_length(input, offset);
						if (!count)
							return unexpected(std::move(count.error()));
						if (*count > (input.size() - offset) / 9U)
							return unexpected(invalid("tuple-count"));
						for (std::uint64_t index{}; index < *count; ++index)
						{
							auto size = read_length(input, offset);
							if (!size)
								return unexpected(std::move(size.error()));
							if (*size == 0U || *size > input.size() - offset)
								return unexpected(invalid("tuple-item-size"));
							if (depth == maximum_nesting_depth_)
								return unexpected(limit("nesting-depth"));
							if (auto valid = preflight(
									input.subspan(offset, static_cast<std::size_t>(*size)),
									depth + 1U);
								!valid)
								return valid;
							offset += static_cast<std::size_t>(*size);
						}
						break;
					}
					default:
						return unexpected(invalid("unknown-tag"));
				}
				if (offset != input.size())
					return unexpected(invalid("trailing-bytes"));
				return {};
			}

		  private:
			[[nodiscard]] error invalid(std::string detail) const
			{
				return {std::string{invalid_error_code_}, "binary", std::move(detail)};
			}

			[[nodiscard]] error limit(std::string detail) const
			{
				return {std::string{limit_error_code_}, "binary", std::move(detail)};
			}

			[[nodiscard]] result<std::uint64_t> read_length(const std::span<const std::byte> input,
															std::size_t& offset) const
			{
				if (offset > input.size() || input.size() - offset < 8U)
					return unexpected(invalid("truncated-length"));
				std::uint64_t value{};
				for (std::size_t index{}; index < 8U; ++index)
					value = (value << 8U) | std::to_integer<unsigned char>(input[offset + index]);
				offset += 8U;
				return value;
			}

			std::size_t maximum_nesting_depth_;
			std::string_view invalid_error_code_;
			std::string_view limit_error_code_;
		};
	} // namespace

	result<canonical_value>
	decode_bounded_canonical_binary(const std::span<const std::byte> input,
									const std::size_t initial_depth,
									const std::size_t maximum_nesting_depth,
									const std::string_view invalid_error_code,
									const std::string_view limit_error_code)
	{
		const decoder bounded_decoder{maximum_nesting_depth, invalid_error_code, limit_error_code};
		if (auto valid = bounded_decoder.preflight(input, initial_depth); !valid)
			return unexpected(std::move(valid.error()));
		auto value = canonical_binary_decode(input);
		if (!value)
			return unexpected(
				error{std::string{invalid_error_code}, "binary", std::move(value.error().detail)});
		return value;
	}
} // namespace cxxlens::sdk::detail
