#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <concepts>
#include <iterator>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <variant>

#include <cxxlens/sdk/provider.hpp>

#include "json_internal.hpp"
#include "provider_runtime_internal.hpp"
#include "provider_validation_internal.hpp"

namespace cxxlens::sdk::provider
{
	namespace
	{
		[[nodiscard]] error
		runtime_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool canonical_digest(const std::string_view value)
		{
			return value.starts_with("sha256:") && value.size() == 71U &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return std::isdigit(static_cast<unsigned char>(byte)) !=
											0 ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool protocol_digest(const std::string_view value)
		{
			constexpr std::string_view semantic_prefix{"semantic-v2:"};
			return canonical_digest(value) ||
				(value.starts_with(semantic_prefix) &&
				 canonical_digest(value.substr(semantic_prefix.size())));
		}

#if !defined(CXXLENS_PROVIDER_RUNTIME_INTERNAL_ONLY)
		[[nodiscard]] std::string json_string(const std::string_view value)
		{
			return cxxlens::sdk::detail::canonical_json_string(value);
		}
#endif

		[[nodiscard]] bool namespaced(const std::string_view value)
		{
			const auto separator = value.find('.');
			return separator != std::string_view::npos && separator != 0U &&
				separator + 1U < value.size();
		}

		[[nodiscard]] bool offered_relation(const manifest& value,
											const std::string_view descriptor)
		{
			return std::ranges::any_of(value.offered_relations,
									   [&](const std::string& offered)
									   {
										   if (offered == descriptor)
											   return true;
										   const auto version = offered.rfind('@');
										   if (version == std::string::npos)
											   return false;
										   return descriptor ==
											   offered.substr(0U, version) + ".v" +
											   offered.substr(version + 1U);
									   });
		}

		[[nodiscard]] const relation_descriptor*
		output_descriptor(const std::span<const relation_descriptor> descriptors,
						  const std::string_view id)
		{
			const auto found = std::ranges::find(descriptors, id, &relation_descriptor::id);
			return found == descriptors.end() ? nullptr : &*found;
		}

		constexpr std::array stable_terminal_reasons{
			std::string_view{"provider.success"},
			std::string_view{"provider.timeout"},
			std::string_view{"provider.cancelled"},
			std::string_view{"provider.output-limit"},
			std::string_view{"provider.crash"},
			std::string_view{"provider.malformed-frame"},
			std::string_view{"provider.checksum-mismatch"},
			std::string_view{"provider.truncated-stream"},
			std::string_view{"provider.schema-invalid"},
			std::string_view{"provider.coverage-incomplete"},
			std::string_view{"provider.runtime-unavailable"},
			std::string_view{"provider.binary-identity-mismatch"},
			std::string_view{"provider.protocol-state-invalid"},
			std::string_view{"provider.credit-exceeded"},
			std::string_view{"provider.backpressure"},
			std::string_view{"provider.task-binding-mismatch"},
			std::string_view{"provider.batch-invalid"},
			std::string_view{"provider.batch-state-invalid"},
			std::string_view{"provider.relation-incompatible"},
			std::string_view{"provider.required-feature-missing"},
			std::string_view{"provider.protocol-minor-mismatch"},
			std::string_view{"provider.unknown-required-extension"},
			std::string_view{"provider.invalid-frame-flags"},
			std::string_view{"provider.unsupported-compression"},
			std::string_view{"provider.frontend-request-invalid"},
			std::string_view{"security.sandbox-insufficient"},
			std::string_view{"security.sandbox-policy-mismatch"},
		};

		[[nodiscard]] bool stable_terminal_reason(const std::string_view reason)
		{
			return std::ranges::find(stable_terminal_reasons, reason) !=
				stable_terminal_reasons.end();
		}

		[[nodiscard]] bool allowed_failure_terminal(const std::string_view reason)
		{
			return reason != "provider.success" && reason.starts_with("provider.") &&
				stable_terminal_reason(reason);
		}

		[[nodiscard]] result<protocol_limits> negotiated_limits(const process_task_request& request)
		{
			const auto& offered = request.selection.selected_candidate().description.protocol;
			const auto minimum =
				std::max<std::uint32_t>(request.limits.minimum_minor, offered.minimum_minor);
			auto maximum =
				std::min<std::uint32_t>(request.limits.maximum_minor, offered.maximum_minor);
			if (request.limits.protocol_major != offered.major || minimum > maximum ||
				maximum > std::numeric_limits<std::uint16_t>::max())
				return cxxlens::sdk::unexpected(
					runtime_error("provider.protocol-minor-mismatch", "negotiation"));
			const bool chunk_feature =
				std::ranges::find(offered.required_features, "task-input-chunks-v1") !=
				offered.required_features.end();
			if (chunk_feature && maximum < 1U)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.required-feature-missing", "task-input-chunks-v1"));
			if (!chunk_feature && maximum >= 1U)
			{
				if (minimum > 0U)
					return cxxlens::sdk::unexpected(
						runtime_error("provider.required-feature-missing", "task-input-chunks-v1"));
				maximum = 0U;
			}
			auto output = request.limits;
			output.minimum_minor = static_cast<std::uint16_t>(maximum);
			output.maximum_minor = static_cast<std::uint16_t>(maximum);
			return output;
		}

		constexpr std::uint64_t canonical_input_chunk_bytes = 1048576U;
		constexpr std::uint64_t maximum_logical_input_bytes = 67108864U;
		constexpr std::uint64_t maximum_input_chunks = 64U;

		class incremental_sha256
		{
		  public:
			void update(std::span<const std::byte> input) noexcept
			{
				total_bytes_ += static_cast<std::uint64_t>(input.size());
				if (pending_size_ != 0U)
				{
					const auto count = std::min(input.size(), block_bytes - pending_size_);
					std::ranges::copy(input.first(count), pending_.begin() + pending_size_);
					pending_size_ += count;
					input = input.subspan(count);
					if (pending_size_ != block_bytes)
						return;
					transform(pending_);
					pending_size_ = 0U;
				}
				while (input.size() >= block_bytes)
				{
					transform(input.first(block_bytes));
					input = input.subspan(block_bytes);
				}
				std::ranges::copy(input, pending_.begin());
				pending_size_ = input.size();
			}

			[[nodiscard]] std::string finish() noexcept
			{
				const auto bit_count = total_bytes_ * 8U;
				pending_[pending_size_++] = std::byte{0x80U};
				if (pending_size_ > 56U)
				{
					std::fill(pending_.begin() + pending_size_, pending_.end(), std::byte{});
					transform(pending_);
					pending_size_ = 0U;
				}
				std::fill(pending_.begin() + pending_size_, pending_.begin() + 56U, std::byte{});
				for (std::size_t index{}; index < 8U; ++index)
					pending_[56U + index] = static_cast<std::byte>(
						(bit_count >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
				transform(pending_);
				constexpr std::string_view digits{"0123456789abcdef"};
				std::string output{"sha256:"};
				output.reserve(71U);
				for (const auto word : state_)
					for (std::uint32_t shift = 28U;; shift -= 4U)
					{
						output.push_back(digits[(word >> shift) & 0x0fU]);
						if (shift == 0U)
							break;
					}
				return output;
			}

		  private:
			static constexpr std::size_t block_bytes = 64U;
			static constexpr std::array<std::uint32_t, 64U> round_constants{
				0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
				0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
				0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
				0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
				0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
				0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
				0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
				0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
				0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
				0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
				0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
			};

			void transform(const std::span<const std::byte> block) noexcept
			{
				std::array<std::uint32_t, 64U> schedule{};
				for (std::size_t index{}; index < 16U; ++index)
				{
					const auto offset = index * 4U;
					schedule[index] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
						(std::to_integer<std::uint32_t>(block[offset + 1U]) << 16U) |
						(std::to_integer<std::uint32_t>(block[offset + 2U]) << 8U) |
						std::to_integer<std::uint32_t>(block[offset + 3U]);
				}
				for (std::size_t index = 16U; index < schedule.size(); ++index)
				{
					const auto small_zero = std::rotr(schedule[index - 15U], 7) ^
						std::rotr(schedule[index - 15U], 18) ^ (schedule[index - 15U] >> 3U);
					const auto small_one = std::rotr(schedule[index - 2U], 17) ^
						std::rotr(schedule[index - 2U], 19) ^ (schedule[index - 2U] >> 10U);
					schedule[index] =
						schedule[index - 16U] + small_zero + schedule[index - 7U] + small_one;
				}
				auto [a, b, c, d, e, f, g, h] = state_;
				for (std::size_t index{}; index < schedule.size(); ++index)
				{
					const auto big_one = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
					const auto choose = (e & f) ^ (~e & g);
					const auto first =
						h + big_one + choose + round_constants[index] + schedule[index];
					const auto big_zero = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
					const auto majority = (a & b) ^ (a & c) ^ (b & c);
					const auto second = big_zero + majority;
					h = g;
					g = f;
					f = e;
					e = d + first;
					d = c;
					c = b;
					b = a;
					a = first + second;
				}
				state_[0U] += a;
				state_[1U] += b;
				state_[2U] += c;
				state_[3U] += d;
				state_[4U] += e;
				state_[5U] += f;
				state_[6U] += g;
				state_[7U] += h;
			}

			std::array<std::uint32_t, 8U> state_{0x6a09e667U,
												 0xbb67ae85U,
												 0x3c6ef372U,
												 0xa54ff53aU,
												 0x510e527fU,
												 0x9b05688cU,
												 0x1f83d9abU,
												 0x5be0cd19U};
			std::array<std::byte, block_bytes> pending_{};
			std::size_t pending_size_{};
			std::uint64_t total_bytes_{};
		};

		using host_cbor_scalar = std::variant<std::uint64_t, std::string>;
		using host_cbor_fields = std::vector<std::pair<std::string, host_cbor_scalar>>;

		template <std::unsigned_integral T>
		void append_host_big_endian(std::vector<std::byte>& output, const T value)
		{
			for (std::size_t index = sizeof(T); index > 0U; --index)
				output.push_back(static_cast<std::byte>(value >> ((index - 1U) * 8U)));
		}

		template <std::unsigned_integral T>
		[[nodiscard]] T read_host_big_endian(const std::span<const std::byte> input,
											 const std::size_t offset)
		{
			T output{};
			for (std::size_t index{}; index < sizeof(T); ++index)
				output = static_cast<T>((output << 8U) |
										std::to_integer<unsigned char>(input[offset + index]));
			return output;
		}

		void append_host_cbor_head(std::vector<std::byte>& output,
								   const std::uint8_t major,
								   const std::uint64_t value)
		{
			const auto prefix = static_cast<std::uint8_t>(major << 5U);
			if (value < 24U)
				output.push_back(static_cast<std::byte>(prefix | static_cast<std::uint8_t>(value)));
			else if (value <= std::numeric_limits<std::uint8_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 24U));
				output.push_back(static_cast<std::byte>(value));
			}
			else if (value <= std::numeric_limits<std::uint16_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 25U));
				append_host_big_endian(output, static_cast<std::uint16_t>(value));
			}
			else if (value <= std::numeric_limits<std::uint32_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 26U));
				append_host_big_endian(output, static_cast<std::uint32_t>(value));
			}
			else
			{
				output.push_back(static_cast<std::byte>(prefix | 27U));
				append_host_big_endian(output, value);
			}
		}

		[[nodiscard]] std::vector<std::byte> host_cbor_text(const std::string_view value)
		{
			std::vector<std::byte> output;
			append_host_cbor_head(output, 3U, value.size());
			for (const auto byte : value)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
			return output;
		}

		[[nodiscard]] result<std::vector<std::byte>>
		encode_host_cbor_map(const host_cbor_fields& fields)
		{
			std::vector<std::tuple<std::vector<std::byte>, std::vector<std::byte>>> encoded;
			std::set<std::string, std::less<>> keys;
			for (const auto& [key, value] : fields)
			{
				if (!keys.insert(key).second || !validate_utf8_text(key))
					return cxxlens::sdk::unexpected(
						runtime_error("provider.host-transcript-invalid", key, "cbor-key"));
				std::vector<std::byte> encoded_value;
				if (const auto* text = std::get_if<std::string>(&value))
				{
					if (!validate_utf8_text(*text))
						return cxxlens::sdk::unexpected(
							runtime_error("provider.host-transcript-invalid", key, "cbor-utf8"));
					encoded_value = host_cbor_text(*text);
				}
				else
					append_host_cbor_head(encoded_value, 0U, std::get<std::uint64_t>(value));
				encoded.emplace_back(host_cbor_text(key), std::move(encoded_value));
			}
			std::ranges::sort(encoded,
							  [](const auto& left, const auto& right)
							  {
								  const auto& lhs = std::get<0>(left);
								  const auto& rhs = std::get<0>(right);
								  return std::pair{lhs.size(), lhs} < std::pair{rhs.size(), rhs};
							  });
			std::vector<std::byte> output;
			append_host_cbor_head(output, 5U, encoded.size());
			for (const auto& [key, value] : encoded)
			{
				output.insert(output.end(), key.begin(), key.end());
				output.insert(output.end(), value.begin(), value.end());
			}
			return output;
		}

		[[nodiscard]] result<std::pair<std::uint64_t, std::size_t>>
		decode_host_cbor_argument(const std::span<const std::byte> input,
								  const std::size_t offset,
								  const std::uint8_t additional)
		{
			if (additional < 24U)
				return std::pair<std::uint64_t, std::size_t>{additional, offset};
			const auto width = additional == 24U ? 1U
				: additional == 25U				 ? 2U
				: additional == 26U				 ? 4U
				: additional == 27U				 ? 8U
												 : 0U;
			if (width == 0U || offset > input.size() || width > input.size() - offset)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "cbor", "argument"));
			std::uint64_t value{};
			for (std::size_t index{}; index < width; ++index)
				value = (value << 8U) | std::to_integer<std::uint64_t>(input[offset + index]);
			const auto minimum = width == 1U ? 24U : (std::uint64_t{1U} << (width * 4U));
			if (value < minimum)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "cbor", "non-shortest"));
			return std::pair{value, offset + width};
		}

		[[nodiscard]] result<std::pair<host_cbor_scalar, std::size_t>>
		decode_host_cbor_scalar(const std::span<const std::byte> input, const std::size_t offset)
		{
			if (offset >= input.size())
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "cbor", "truncated"));
			const auto initial = std::to_integer<std::uint8_t>(input[offset]);
			const auto major = initial >> 5U;
			if (major != 0U && major != 3U)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "cbor", "scalar-type"));
			auto argument = decode_host_cbor_argument(input, offset + 1U, initial & 0x1fU);
			if (!argument)
				return cxxlens::sdk::unexpected(std::move(argument.error()));
			if (major == 0U)
				return std::pair{host_cbor_scalar{argument->first}, argument->second};
			if (argument->first > input.size() - argument->second)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "cbor", "text-length"));
			const std::string text{reinterpret_cast<const char*>(input.data() + argument->second),
								   static_cast<std::size_t>(argument->first)};
			if (!validate_utf8_text(text))
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "cbor", "utf8"));
			return std::pair{host_cbor_scalar{text}, argument->second + text.size()};
		}

		[[nodiscard]] result<std::map<std::string, host_cbor_scalar, std::less<>>>
		decode_host_cbor_map(const std::span<const std::byte> input)
		{
			if (input.empty() || (std::to_integer<std::uint8_t>(input.front()) >> 5U) != 5U)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "cbor", "map"));
			const auto initial = std::to_integer<std::uint8_t>(input.front());
			auto count = decode_host_cbor_argument(input, 1U, initial & 0x1fU);
			if (!count)
				return cxxlens::sdk::unexpected(std::move(count.error()));
			std::map<std::string, host_cbor_scalar, std::less<>> output;
			std::vector<std::byte> previous_key;
			auto offset = count->second;
			for (std::uint64_t index{}; index < count->first; ++index)
			{
				const auto key_begin = offset;
				auto key = decode_host_cbor_scalar(input, offset);
				if (!key || !std::holds_alternative<std::string>(key->first))
					return cxxlens::sdk::unexpected(
						runtime_error("provider.host-transcript-invalid", "cbor", "map-key"));
				offset = key->second;
				std::vector encoded_key(input.begin() + static_cast<std::ptrdiff_t>(key_begin),
										input.begin() + static_cast<std::ptrdiff_t>(offset));
				if (!previous_key.empty() &&
					std::pair{encoded_key.size(), encoded_key} <=
						std::pair{previous_key.size(), previous_key})
					return cxxlens::sdk::unexpected(
						runtime_error("provider.host-transcript-invalid", "cbor", "map-order"));
				previous_key = std::move(encoded_key);
				auto value = decode_host_cbor_scalar(input, offset);
				if (!value)
					return cxxlens::sdk::unexpected(std::move(value.error()));
				offset = value->second;
				if (!output
						 .emplace(std::get<std::string>(std::move(key->first)),
								  std::move(value->first))
						 .second)
					return cxxlens::sdk::unexpected(
						runtime_error("provider.host-transcript-invalid", "cbor", "duplicate-key"));
			}
			if (offset != input.size())
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "cbor", "trailing"));
			return output;
		}

		template <typename T>
		[[nodiscard]] const T*
		host_cbor_field(const std::map<std::string, host_cbor_scalar, std::less<>>& fields,
						const std::string_view key)
		{
			const auto found = fields.find(key);
			return found == fields.end() ? nullptr : std::get_if<T>(&found->second);
		}

		struct input_descriptor_record
		{
			std::string task_id;
			std::string input_digest;
			std::uint64_t total_bytes{};
			std::uint64_t chunk_bytes{};
			std::uint64_t chunk_count{};
		};

		struct input_chunk_record
		{
			std::string task_id;
			std::string input_digest;
			std::uint64_t chunk_index{};
			std::uint64_t offset{};
			std::uint64_t byte_count{};
		};

		[[nodiscard]] result<std::vector<std::byte>>
		encode_input_descriptor(const input_descriptor_record& value)
		{
			return encode_host_cbor_map({
				{"schema", std::string{"cxxlens.provider-control.input-descriptor.v1"}},
				{"task_id", value.task_id},
				{"input_digest", value.input_digest},
				{"total_bytes", value.total_bytes},
				{"chunk_bytes", value.chunk_bytes},
				{"chunk_count", value.chunk_count},
			});
		}

		[[nodiscard]] result<input_descriptor_record>
		decode_input_descriptor(const std::span<const std::byte> control)
		{
			auto fields = decode_host_cbor_map(control);
			if (!fields || fields->size() != 6U)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "input_descriptor", "shape"));
			const auto* schema = host_cbor_field<std::string>(*fields, "schema");
			const auto* task_id = host_cbor_field<std::string>(*fields, "task_id");
			const auto* input_digest = host_cbor_field<std::string>(*fields, "input_digest");
			const auto* total_bytes = host_cbor_field<std::uint64_t>(*fields, "total_bytes");
			const auto* chunk_bytes = host_cbor_field<std::uint64_t>(*fields, "chunk_bytes");
			const auto* chunk_count = host_cbor_field<std::uint64_t>(*fields, "chunk_count");
			if (schema == nullptr || *schema != "cxxlens.provider-control.input-descriptor.v1" ||
				task_id == nullptr || input_digest == nullptr || total_bytes == nullptr ||
				chunk_bytes == nullptr || chunk_count == nullptr)
				return cxxlens::sdk::unexpected(runtime_error(
					"provider.host-transcript-invalid", "input_descriptor", "fields"));
			return input_descriptor_record{
				*task_id, *input_digest, *total_bytes, *chunk_bytes, *chunk_count};
		}

		[[nodiscard]] result<std::vector<std::byte>>
		encode_input_chunk(const input_chunk_record& value)
		{
			return encode_host_cbor_map({
				{"schema", std::string{"cxxlens.provider-control.input-chunk.v1"}},
				{"task_id", value.task_id},
				{"input_digest", value.input_digest},
				{"chunk_index", value.chunk_index},
				{"offset", value.offset},
				{"byte_count", value.byte_count},
			});
		}

		[[nodiscard]] result<input_chunk_record>
		decode_input_chunk(const std::span<const std::byte> control)
		{
			auto fields = decode_host_cbor_map(control);
			if (!fields || fields->size() != 6U)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "input_chunk", "shape"));
			const auto* schema = host_cbor_field<std::string>(*fields, "schema");
			const auto* task_id = host_cbor_field<std::string>(*fields, "task_id");
			const auto* input_digest = host_cbor_field<std::string>(*fields, "input_digest");
			const auto* chunk_index = host_cbor_field<std::uint64_t>(*fields, "chunk_index");
			const auto* offset = host_cbor_field<std::uint64_t>(*fields, "offset");
			const auto* byte_count = host_cbor_field<std::uint64_t>(*fields, "byte_count");
			if (schema == nullptr || *schema != "cxxlens.provider-control.input-chunk.v1" ||
				task_id == nullptr || input_digest == nullptr || chunk_index == nullptr ||
				offset == nullptr || byte_count == nullptr)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.host-transcript-invalid", "input_chunk", "fields"));
			return input_chunk_record{*task_id, *input_digest, *chunk_index, *offset, *byte_count};
		}

		[[nodiscard]] result<void> validate_host_profile(const detail::host_input_profile& profile)
		{
			const auto& expected = profile.expectation;
			const auto minor = expected.limits.maximum_minor;
			if (expected.provider_manifest.empty() || expected.task.task_id.empty() ||
				expected.task.task_id.contains('\0') ||
				!canonical_digest(expected.task.task_input_digest) ||
				!canonical_digest(expected.task.normalized_invocation_digest) ||
				!canonical_digest(expected.task.toolchain_digest) ||
				!canonical_digest(expected.task.environment_digest) ||
				expected.limits.protocol_major != 1U || minor > 1U ||
				expected.limits.minimum_minor > minor ||
				profile.task_input_chunks_v1 != (minor == 1U) ||
				expected.limits.max_control_bytes == 0U ||
				expected.limits.max_payload_bytes == 0U ||
				(profile.task_input_chunks_v1 &&
				 expected.limits.max_payload_bytes < canonical_input_chunk_bytes))
				return cxxlens::sdk::unexpected(runtime_error(
					"provider.host-transcript-invalid", expected.task.task_id, "profile"));
			return {};
		}

		struct host_input_state_result
		{
			open_task_metadata task;
			protocol_credit credit;
			std::uint64_t total_bytes{};
			std::uint64_t chunk_bytes{};
			std::vector<std::string> ordered_chunk_digests;
		};

		class host_input_state
		{
		  public:
			host_input_state(const detail::host_input_profile& profile,
							 detail::host_input_chunk_sink& sink)
				: profile_{profile}, sink_{sink}
			{
			}

			[[nodiscard]] result<void> consume(const frame& value)
			{
				const auto fail = [&](const std::string_view detail)
				{
					return result<void>{
						cxxlens::sdk::unexpected(runtime_error("provider.host-transcript-invalid",
															   profile_.expectation.task.task_id,
															   std::string{detail}))};
				};
				const auto& expected = profile_.expectation;
				if (closed_ || value.stream_id != 1U || value.sequence != next_sequence_ ||
					value.flags != 0U || value.protocol_major != expected.limits.protocol_major ||
					value.protocol_minor != expected.limits.maximum_minor ||
					value.control.empty() ||
					value.control.size() > expected.limits.max_control_bytes ||
					value.payload.size() > expected.limits.max_payload_bytes)
					return fail("frame-state");

				if (next_sequence_ == 0U)
				{
					auto manifest = decode_control_text(value.control);
					if (value.type != message_type::hello_ack || !manifest ||
						*manifest != expected.provider_manifest || !value.payload.empty())
						return fail("hello-ack");
				}
				else if (next_sequence_ == 1U)
				{
					auto schema = decode_schema_negotiate_metadata(value.control);
					if (value.type != message_type::schema_negotiate || !schema ||
						schema->protocol_schema != "cxxlens.provider-protocol.v1" ||
						schema->protocol_minor != expected.limits.maximum_minor ||
						!value.payload.empty())
						return fail("schema");
				}
				else if (next_sequence_ == 2U)
				{
					auto task = decode_open_task_metadata(value.control);
					if (value.type != message_type::open_task || !task || *task != expected.task ||
						(profile_.task_input_chunks_v1 && !value.payload.empty()))
						return fail("open-task");
					task_ = std::move(*task);
					if (!profile_.task_input_chunks_v1)
					{
						if (value.payload.size() > expected.limits.max_payload_bytes)
							return fail("inline-limit");
						if (auto written = sink_.append(value.payload); !written)
							return written;
						hash_.update(value.payload);
						total_bytes_ = value.payload.size();
						chunk_bytes_ = value.payload.size();
						if (!value.payload.empty())
							chunk_digests_.push_back(content_digest(value.payload));
					}
				}
				else if (profile_.task_input_chunks_v1 && next_sequence_ == 3U)
				{
					auto descriptor = decode_input_descriptor(value.control);
					if (value.type != message_type::input_descriptor || !descriptor ||
						!value.payload.empty() || descriptor->task_id != expected.task.task_id ||
						descriptor->input_digest != expected.task.task_input_digest ||
						descriptor->total_bytes > maximum_logical_input_bytes ||
						descriptor->chunk_bytes == 0U ||
						descriptor->chunk_bytes > canonical_input_chunk_bytes)
						return fail("input-descriptor");
					const auto expected_count = descriptor->total_bytes == 0U
						? 0U
						: 1U + ((descriptor->total_bytes - 1U) / descriptor->chunk_bytes);
					if (descriptor->chunk_count != expected_count ||
						descriptor->chunk_count > maximum_input_chunks)
						return fail("input-descriptor-count");
					total_bytes_ = descriptor->total_bytes;
					chunk_bytes_ = descriptor->chunk_bytes;
					chunk_count_ = descriptor->chunk_count;
					descriptor_seen_ = true;
				}
				else if (profile_.task_input_chunks_v1 && descriptor_seen_ &&
						 next_chunk_index_ < chunk_count_ &&
						 next_sequence_ == 4U + next_chunk_index_)
				{
					auto chunk = decode_input_chunk(value.control);
					const auto expected_bytes =
						std::min(chunk_bytes_, total_bytes_ - received_bytes_);
					if (value.type != message_type::input_chunk || !chunk ||
						chunk->task_id != expected.task.task_id ||
						chunk->input_digest != expected.task.task_input_digest ||
						chunk->chunk_index != next_chunk_index_ ||
						chunk->offset != received_bytes_ || chunk->byte_count != expected_bytes ||
						expected_bytes == 0U || value.payload.size() != expected_bytes ||
						(next_chunk_index_ + 1U < chunk_count_ && expected_bytes != chunk_bytes_))
						return fail("input-chunk");
					if (auto written = sink_.append(value.payload); !written)
						return written;
					hash_.update(value.payload);
					chunk_digests_.push_back(content_digest(value.payload));
					received_bytes_ += expected_bytes;
					++next_chunk_index_;
				}
				else if ((!profile_.task_input_chunks_v1 && next_sequence_ == 3U) ||
						 (profile_.task_input_chunks_v1 && descriptor_seen_ &&
						  next_chunk_index_ == chunk_count_ && next_sequence_ == 4U + chunk_count_))
				{
					auto credit = decode_credit_metadata(value.control);
					if (value.type != message_type::credit || !credit || credit->bytes == 0U ||
						credit->frames == 0U || !value.payload.empty())
						return fail("credit");
					credit_ = {credit->bytes, credit->frames};
					credit_seen_ = true;
				}
				else if ((!profile_.task_input_chunks_v1 && next_sequence_ == 4U) ||
						 (profile_.task_input_chunks_v1 && credit_seen_ &&
						  next_sequence_ == 5U + chunk_count_))
				{
					auto close = decode_close_metadata(value.control);
					if (value.type != message_type::close || !close ||
						close->task_id != expected.task.task_id || !value.payload.empty())
						return fail("close");
					closed_ = true;
				}
				else
					return fail("transition");
				++next_sequence_;
				return {};
			}

			[[nodiscard]] result<host_input_state_result> finish()
			{
				const auto& expected = profile_.expectation;
				const auto expected_frames = profile_.task_input_chunks_v1 ? 6U + chunk_count_ : 5U;
				if (!closed_ || !credit_seen_ || !task_ || next_sequence_ != expected_frames ||
					(profile_.task_input_chunks_v1 &&
					 (!descriptor_seen_ || next_chunk_index_ != chunk_count_ ||
					  received_bytes_ != total_bytes_)))
					return cxxlens::sdk::unexpected(runtime_error(
						"provider.host-transcript-invalid", expected.task.task_id, "incomplete"));
				const auto digest = hash_.finish();
				if (digest != expected.task.task_input_digest)
					return cxxlens::sdk::unexpected(runtime_error(
						"provider.host-transcript-invalid", expected.task.task_id, "input-digest"));
				return host_input_state_result{std::move(*task_),
											   credit_,
											   total_bytes_,
											   chunk_bytes_,
											   std::move(chunk_digests_)};
			}

		  private:
			const detail::host_input_profile& profile_;
			detail::host_input_chunk_sink& sink_;
			std::uint64_t next_sequence_{};
			std::optional<open_task_metadata> task_;
			protocol_credit credit_{};
			bool descriptor_seen_{};
			bool credit_seen_{};
			bool closed_{};
			std::uint64_t total_bytes_{};
			std::uint64_t chunk_bytes_{};
			std::uint64_t chunk_count_{};
			std::uint64_t next_chunk_index_{};
			std::uint64_t received_bytes_{};
			incremental_sha256 hash_;
			std::vector<std::string> chunk_digests_;
		};

		[[nodiscard]] result<std::string>
		ordered_input_chunk_set_digest(const std::string_view task_id,
									   const std::string_view input_digest,
									   const std::span<const std::string> chunk_digests)
		{
			std::vector<canonical_value> chunks;
			chunks.reserve(chunk_digests.size());
			for (const auto& digest : chunk_digests)
				chunks.push_back(canonical_value::from_string(digest));
			auto projection = canonical_binary(canonical_value::from_tuple({
				canonical_value::from_tuple({canonical_value::from_string("chunk_digests"),
											 canonical_value::from_tuple(std::move(chunks))}),
				canonical_value::from_tuple(
					{canonical_value::from_string("input_digest"),
					 canonical_value::from_string(std::string{input_digest})}),
				canonical_value::from_tuple({canonical_value::from_string("task_id"),
											 canonical_value::from_string(std::string{task_id})}),
			}));
			if (!projection)
				return cxxlens::sdk::unexpected(std::move(projection.error()));
			const std::string bytes{reinterpret_cast<const char*>(projection->data()),
									projection->size()};
			return semantic_digest("cxxlens.provider-input-chunk-payload-set.v1", bytes);
		}

		[[nodiscard]] canonical_value
		runtime_string_tuple(const std::span<const std::string> values)
		{
			std::vector<canonical_value> output;
			output.reserve(values.size());
			for (const auto& value : values)
				output.push_back(canonical_value::from_string(value));
			return canonical_value::from_tuple(std::move(output));
		}

		[[nodiscard]] result<std::string> runtime_projection_digest(const std::string_view domain,
																	canonical_value projection)
		{
			auto encoded = canonical_binary(projection);
			if (!encoded)
				return cxxlens::sdk::unexpected(std::move(encoded.error()));
			const std::string bytes{reinterpret_cast<const char*>(encoded->data()),
									encoded->size()};
			return semantic_digest(domain, bytes);
		}

		[[nodiscard]] result<std::string>
		frame_transcript_receipt_digest(const std::span<const frame> frames)
		{
			std::vector<canonical_value> projected;
			projected.reserve(frames.size());
			for (std::size_t index{}; index < frames.size(); ++index)
			{
				const auto& value = frames[index];
				if (value.sequence != index ||
					value.stream_id >
						static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
					value.sequence >
						static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					return cxxlens::sdk::unexpected(runtime_error(
						"provider.protocol-state-invalid", "runtime-receipt", "frame-projection"));
				projected.push_back(canonical_value::from_tuple({
					canonical_value::from_integer(value.protocol_major),
					canonical_value::from_integer(value.protocol_minor),
					canonical_value::from_integer(value.flags),
					canonical_value::from_integer(static_cast<std::uint16_t>(value.type)),
					canonical_value::from_integer(static_cast<std::int64_t>(value.stream_id)),
					canonical_value::from_integer(static_cast<std::int64_t>(value.sequence)),
					canonical_value::from_string(content_digest(value.control)),
					canonical_value::from_string(content_digest(value.payload)),
				}));
			}
			return runtime_projection_digest("cxxlens.provider-frame-transcript.v2",
											 canonical_value::from_tuple(std::move(projected)));
		}

		[[nodiscard]] result<std::string>
		sealed_transcript_receipt_digest(const std::string_view task_id,
										 const std::string_view terminal,
										 const detail::sealed_provider_transcript& sealed)
		{
			if (task_id.empty() || terminal.empty())
				return cxxlens::sdk::unexpected(runtime_error(
					"provider.protocol-state-invalid", "runtime-receipt", "terminal-projection"));
			std::vector<canonical_value> batches;
			batches.reserve(sealed.batches().size());
			for (const auto& batch : sealed.batches())
			{
				if (batch.task_id() != task_id)
					return cxxlens::sdk::unexpected(runtime_error(
						"provider.protocol-state-invalid", "runtime-receipt", "batch-task"));
				std::vector<std::string> row_forms;
				row_forms.reserve(batch.rows().size());
				for (const auto& row : batch.rows())
					row_forms.push_back(row.canonical_form());
				batches.push_back(canonical_value::from_tuple({
					canonical_value::from_string(std::string{batch.task_id()}),
					canonical_value::from_string(std::string{batch.descriptor_id()}),
					canonical_value::from_string(std::string{batch.descriptor_digest()}),
					canonical_value::from_string(std::string{batch.dependency_group_id()}),
					canonical_value::from_string(std::string{batch.atomic_output_group_id()}),
					canonical_value::from_string(std::string{batch.batch_id()}),
					canonical_value::from_string(std::string{batch.batch_digest()}),
					runtime_string_tuple(batch.ordered_chunk_digests()),
					runtime_string_tuple(row_forms),
				}));
			}
			std::vector<canonical_value> coverage;
			coverage.reserve(sealed.coverage().size());
			for (const auto& record : sealed.coverage())
				coverage.push_back(canonical_value::from_tuple({
					canonical_value::from_string(record.kind),
					canonical_value::from_string(record.id),
					canonical_value::from_string(record.state),
					canonical_value::from_string(record.reason),
				}));
			std::vector<canonical_value> unresolved;
			unresolved.reserve(sealed.unresolved().size());
			for (const auto& record : sealed.unresolved())
				unresolved.push_back(canonical_value::from_tuple({
					canonical_value::from_string(record.code),
					canonical_value::from_string(record.subject),
					canonical_value::from_string(record.detail),
				}));
			std::vector<canonical_value> evidence;
			evidence.reserve(sealed.evidence().size());
			for (const auto& record : sealed.evidence())
				evidence.push_back(canonical_value::from_tuple({
					canonical_value::from_string(record.kind),
					canonical_value::from_string(record.subject),
					canonical_value::from_string(record.producer),
					canonical_value::from_string(record.summary),
				}));
			return runtime_projection_digest(
				"cxxlens.provider-sealed-transcript.v1",
				canonical_value::from_tuple({
					canonical_value::from_string(std::string{task_id}),
					canonical_value::from_string(std::string{terminal}),
					canonical_value::from_tuple(std::move(batches)),
					canonical_value::from_tuple(std::move(coverage)),
					canonical_value::from_tuple(std::move(unresolved)),
					canonical_value::from_tuple(std::move(evidence)),
				}));
		}
	} // namespace

	namespace detail
	{
		provider_runtime_receipt::provider_runtime_receipt(
			const std::uint64_t raw_stdout_byte_count,
			std::string raw_stdout_sha256,
			const std::uint64_t decoded_frame_count,
			std::string frame_transcript_digest,
			std::string sealed_transcript_digest)
			: raw_stdout_byte_count_{raw_stdout_byte_count},
			  raw_stdout_sha256_{std::move(raw_stdout_sha256)},
			  decoded_frame_count_{decoded_frame_count},
			  frame_transcript_digest_{std::move(frame_transcript_digest)},
			  sealed_transcript_digest_{std::move(sealed_transcript_digest)}
		{
		}

		std::uint64_t provider_runtime_receipt::raw_stdout_byte_count() const noexcept
		{
			return raw_stdout_byte_count_;
		}
		std::string_view provider_runtime_receipt::raw_stdout_sha256() const noexcept
		{
			return raw_stdout_sha256_;
		}
		std::uint64_t provider_runtime_receipt::decoded_frame_count() const noexcept
		{
			return decoded_frame_count_;
		}
		std::string_view provider_runtime_receipt::frame_transcript_digest() const noexcept
		{
			return frame_transcript_digest_;
		}
		std::string_view provider_runtime_receipt::sealed_transcript_digest() const noexcept
		{
			return sealed_transcript_digest_;
		}

		result<provider_runtime_receipt>
		make_provider_runtime_receipt(const std::uint64_t raw_stdout_byte_count,
									  std::string raw_stdout_sha256,
									  const std::span<const frame> frames,
									  const std::string_view task_id,
									  const std::string_view terminal,
									  const sealed_provider_transcript& sealed)
		{
			if (!canonical_digest(raw_stdout_sha256) || raw_stdout_byte_count == 0U ||
				frames.empty())
				return cxxlens::sdk::unexpected(runtime_error(
					"provider.protocol-state-invalid", "runtime-receipt", "raw-observation"));
			auto frame_digest = frame_transcript_receipt_digest(frames);
			if (!frame_digest)
				return cxxlens::sdk::unexpected(std::move(frame_digest.error()));
			auto sealed_digest = sealed_transcript_receipt_digest(task_id, terminal, sealed);
			if (!sealed_digest)
				return cxxlens::sdk::unexpected(std::move(sealed_digest.error()));
			return provider_runtime_receipt{raw_stdout_byte_count,
											std::move(raw_stdout_sha256),
											frames.size(),
											std::move(*frame_digest),
											std::move(*sealed_digest)};
		}

		result<void> expected_provider_identity::validate() const
		{
			const auto ordered_unique = [](const std::span<const std::string> values)
			{
				return std::ranges::all_of(values,
										   [](const std::string& value)
										   {
											   return !value.empty() && !value.contains('\0');
										   }) &&
					std::ranges::is_sorted(values) &&
					std::ranges::adjacent_find(values) == values.end();
			};
			if (provider_id.empty() || provider_id.contains('\0') || provider_version.major == 0U ||
				!canonical_digest(provider_binary_digest) ||
				!canonical_digest(provider_semantic_contract_digest) || protocol_major != 1U ||
				protocol_minor > 1U || !ordered_unique(required_features) ||
				!protocol_digest(sandbox_policy_digest) || !ordered_unique(offered_relations) ||
				(protocol_minor == 1U) !=
					(std::ranges::find(required_features, "task-input-chunks-v1") !=
					 required_features.end()))
				return cxxlens::sdk::unexpected(runtime_error(
					"provider.binary-identity-mismatch", provider_id, "expected-provider-invalid"));
			return {};
		}

		sealed_host_input::sealed_host_input(open_task_metadata task,
											 protocol_credit credit,
											 const std::uint16_t protocol_major,
											 const std::uint16_t protocol_minor,
											 const std::uint64_t total_bytes,
											 const std::uint64_t chunk_bytes,
											 std::vector<std::string> ordered_chunk_digests,
											 std::string ordered_chunk_digest_set_digest)
			: task_{std::move(task)}, credit_{credit}, protocol_major_{protocol_major},
			  protocol_minor_{protocol_minor}, total_bytes_{total_bytes}, chunk_bytes_{chunk_bytes},
			  ordered_chunk_digests_{std::move(ordered_chunk_digests)},
			  ordered_chunk_digest_set_digest_{std::move(ordered_chunk_digest_set_digest)}
		{
		}

		const open_task_metadata& sealed_host_input::task() const noexcept
		{
			return task_;
		}
		protocol_credit sealed_host_input::credit() const noexcept
		{
			return credit_;
		}
		std::uint16_t sealed_host_input::protocol_major() const noexcept
		{
			return protocol_major_;
		}
		std::uint16_t sealed_host_input::protocol_minor() const noexcept
		{
			return protocol_minor_;
		}
		std::uint64_t sealed_host_input::total_bytes() const noexcept
		{
			return total_bytes_;
		}
		std::uint64_t sealed_host_input::chunk_bytes() const noexcept
		{
			return chunk_bytes_;
		}
		std::uint64_t sealed_host_input::chunk_count() const noexcept
		{
			return ordered_chunk_digests_.size();
		}
		std::span<const std::string> sealed_host_input::ordered_chunk_digests() const noexcept
		{
			return ordered_chunk_digests_;
		}
		std::string_view sealed_host_input::ordered_chunk_digest_set_digest() const noexcept
		{
			return ordered_chunk_digest_set_digest_;
		}

		result<sealed_host_input>
		encode_host_transcript_incremental(const host_input_profile& profile,
										   const protocol_credit credit,
										   const replayable_host_input& input,
										   frame_sink& output)
		{
			if (auto valid = validate_host_profile(profile); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			if (credit.bytes == 0U || credit.frames == 0U)
				return cxxlens::sdk::unexpected(runtime_error("provider.host-transcript-invalid",
															  profile.expectation.task.task_id,
															  "credit"));

			auto input_size = input.size();
			if (!input_size)
				return cxxlens::sdk::unexpected(std::move(input_size.error()));
			const auto size_limit = profile.task_input_chunks_v1
				? maximum_logical_input_bytes
				: profile.expectation.limits.max_payload_bytes;
			if (*input_size > size_limit || *input_size > std::numeric_limits<std::size_t>::max())
				return cxxlens::sdk::unexpected(runtime_error("provider.host-transcript-invalid",
															  profile.expectation.task.task_id,
															  "input-limit"));

			const auto read_exact =
				[&](const std::uint64_t offset, const std::span<std::byte> bytes)
			{
				std::size_t received{};
				while (received < bytes.size())
				{
					auto count = input.read_at(offset + received, bytes.subspan(received));
					if (!count)
						return result<void>{cxxlens::sdk::unexpected(std::move(count.error()))};
					if (*count == 0U || *count > bytes.size() - received)
						return result<void>{cxxlens::sdk::unexpected(
							runtime_error("provider.host-transcript-invalid",
										  profile.expectation.task.task_id,
										  "short-read"))};
					received += *count;
				}
				return result<void>{};
			};

			incremental_sha256 first_pass_hash;
			std::vector<std::byte> hash_buffer(static_cast<std::size_t>(std::min<std::uint64_t>(
				canonical_input_chunk_bytes, std::max<std::uint64_t>(*input_size, 1U))));
			for (std::uint64_t offset{}; offset < *input_size;)
			{
				const auto count = static_cast<std::size_t>(
					std::min<std::uint64_t>(hash_buffer.size(), *input_size - offset));
				if (auto read = read_exact(offset, std::span{hash_buffer}.first(count)); !read)
					return cxxlens::sdk::unexpected(std::move(read.error()));
				first_pass_hash.update(std::span{hash_buffer}.first(count));
				offset += count;
			}
			if (first_pass_hash.finish() != profile.expectation.task.task_input_digest)
				return cxxlens::sdk::unexpected(runtime_error("provider.host-transcript-invalid",
															  profile.expectation.task.task_id,
															  "input-digest"));

			class discard_input_sink final : public host_input_chunk_sink
			{
			  public:
				result<void> append(std::span<const std::byte>) override
				{
					return {};
				}
			} discard;
			host_input_state state{profile, discard};
			const auto send = [&](frame value) -> result<void>
			{
				value.protocol_major = profile.expectation.limits.protocol_major;
				value.protocol_minor = profile.expectation.limits.maximum_minor;
				if (auto valid = state.consume(value); !valid)
					return valid;
				auto encoded = encode_frame(value, profile.expectation.limits);
				if (!encoded)
					return cxxlens::sdk::unexpected(std::move(encoded.error()));
				return output.write(*encoded);
			};

			auto hello = encode_control_text(profile.expectation.provider_manifest);
			auto schema = encode_schema_negotiate_metadata(
				{"cxxlens.provider-protocol.v1", profile.expectation.limits.maximum_minor});
			auto open = encode_open_task_metadata(profile.expectation.task);
			auto output_credit = encode_credit_metadata({credit.bytes, credit.frames});
			auto close = encode_close_metadata({profile.expectation.task.task_id});
			if (!hello || !schema || !open || !output_credit || !close)
				return cxxlens::sdk::unexpected(runtime_error("provider.host-transcript-invalid",
															  profile.expectation.task.task_id,
															  "control-encoding"));
			if (auto written = send({message_type::hello_ack, 1U, 0U, std::move(*hello), {}});
				!written)
				return cxxlens::sdk::unexpected(std::move(written.error()));
			if (auto written =
					send({message_type::schema_negotiate, 1U, 1U, std::move(*schema), {}});
				!written)
				return cxxlens::sdk::unexpected(std::move(written.error()));

			std::uint64_t sequence = 2U;
			if (!profile.task_input_chunks_v1)
			{
				std::vector<std::byte> payload(static_cast<std::size_t>(*input_size));
				if (auto read = read_exact(0U, payload); !read)
					return cxxlens::sdk::unexpected(std::move(read.error()));
				if (auto written = send({message_type::open_task,
										 1U,
										 sequence++,
										 std::move(*open),
										 std::move(payload)});
					!written)
					return cxxlens::sdk::unexpected(std::move(written.error()));
			}
			else
			{
				if (auto written =
						send({message_type::open_task, 1U, sequence++, std::move(*open), {}});
					!written)
					return cxxlens::sdk::unexpected(std::move(written.error()));
				const auto count = *input_size == 0U
					? 0U
					: 1U + ((*input_size - 1U) / canonical_input_chunk_bytes);
				auto descriptor =
					encode_input_descriptor({profile.expectation.task.task_id,
											 profile.expectation.task.task_input_digest,
											 *input_size,
											 canonical_input_chunk_bytes,
											 count});
				if (!descriptor)
					return cxxlens::sdk::unexpected(std::move(descriptor.error()));
				if (auto written = send({message_type::input_descriptor,
										 1U,
										 sequence++,
										 std::move(*descriptor),
										 {}});
					!written)
					return cxxlens::sdk::unexpected(std::move(written.error()));
				for (std::uint64_t index{}; index < count; ++index)
				{
					const auto offset = index * canonical_input_chunk_bytes;
					const auto byte_count =
						std::min<std::uint64_t>(canonical_input_chunk_bytes, *input_size - offset);
					std::vector<std::byte> payload(static_cast<std::size_t>(byte_count));
					if (auto read = read_exact(offset, payload); !read)
						return cxxlens::sdk::unexpected(std::move(read.error()));
					auto control = encode_input_chunk({profile.expectation.task.task_id,
													   profile.expectation.task.task_input_digest,
													   index,
													   offset,
													   byte_count});
					if (!control)
						return cxxlens::sdk::unexpected(std::move(control.error()));
					if (auto written = send({message_type::input_chunk,
											 1U,
											 sequence++,
											 std::move(*control),
											 std::move(payload)});
						!written)
						return cxxlens::sdk::unexpected(std::move(written.error()));
				}
			}
			if (auto written =
					send({message_type::credit, 1U, sequence++, std::move(*output_credit), {}});
				!written)
				return cxxlens::sdk::unexpected(std::move(written.error()));
			if (auto written = send({message_type::close, 1U, sequence, std::move(*close), {}});
				!written)
				return cxxlens::sdk::unexpected(std::move(written.error()));
			auto sealed = state.finish();
			if (!sealed)
				return cxxlens::sdk::unexpected(std::move(sealed.error()));
			auto digest = ordered_input_chunk_set_digest(sealed->task.task_id,
														 sealed->task.task_input_digest,
														 sealed->ordered_chunk_digests);
			if (!digest)
				return cxxlens::sdk::unexpected(std::move(digest.error()));
			return sealed_host_input{std::move(sealed->task),
									 sealed->credit,
									 profile.expectation.limits.protocol_major,
									 profile.expectation.limits.maximum_minor,
									 sealed->total_bytes,
									 sealed->chunk_bytes,
									 std::move(sealed->ordered_chunk_digests),
									 std::move(*digest)};
		}

		result<sealed_host_input>
		validate_host_transcript_incremental(const std::span<const frame> frames,
											 const host_input_profile& profile,
											 host_input_chunk_sink& input)
		{
			if (auto valid = validate_host_profile(profile); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			host_input_state state{profile, input};
			for (const auto& value : frames)
				if (auto valid = state.consume(value); !valid)
					return cxxlens::sdk::unexpected(std::move(valid.error()));
			auto sealed = state.finish();
			if (!sealed)
				return cxxlens::sdk::unexpected(std::move(sealed.error()));
			auto digest = ordered_input_chunk_set_digest(sealed->task.task_id,
														 sealed->task.task_input_digest,
														 sealed->ordered_chunk_digests);
			if (!digest)
				return cxxlens::sdk::unexpected(std::move(digest.error()));
			return sealed_host_input{std::move(sealed->task),
									 sealed->credit,
									 profile.expectation.limits.protocol_major,
									 profile.expectation.limits.maximum_minor,
									 sealed->total_bytes,
									 sealed->chunk_bytes,
									 std::move(sealed->ordered_chunk_digests),
									 std::move(*digest)};
		}

		result<sealed_host_input> validate_host_transcript_stream(host_input_byte_source& source,
																  const host_input_profile& profile,
																  host_input_chunk_sink& input)
		{
			if (auto valid = validate_host_profile(profile); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			const auto read_exact = [&](const std::span<std::byte> output,
										const bool clean_eof_allowed) -> result<bool>
			{
				std::size_t received{};
				while (received < output.size())
				{
					auto count = source.read(output.subspan(received));
					if (!count)
						return cxxlens::sdk::unexpected(std::move(count.error()));
					if (*count > output.size() - received)
						return cxxlens::sdk::unexpected(
							runtime_error("provider.host-transcript-invalid",
										  profile.expectation.task.task_id,
										  "source-overread"));
					if (*count == 0U)
					{
						if (received == 0U && clean_eof_allowed)
							return true;
						return cxxlens::sdk::unexpected(
							runtime_error("provider.truncated-stream",
										  profile.expectation.task.task_id,
										  "host-input"));
					}
					received += *count;
				}
				return false;
			};

			constexpr std::size_t wire_header_bytes = 104U;
			host_input_state state{profile, input};
			for (;;)
			{
				std::array<std::byte, wire_header_bytes> header{};
				auto eof = read_exact(header, true);
				if (!eof)
					return cxxlens::sdk::unexpected(std::move(eof.error()));
				if (*eof)
					break;
				const auto control_bytes = read_host_big_endian<std::uint32_t>(header, 28U);
				const auto payload_bytes = read_host_big_endian<std::uint64_t>(header, 32U);
				const auto wire_type = read_host_big_endian<std::uint16_t>(header, 8U);
				const auto profile_payload_limit = profile.task_input_chunks_v1
					? (wire_type == static_cast<std::uint16_t>(message_type::input_chunk)
						   ? canonical_input_chunk_bytes
						   : 0U)
					: profile.expectation.limits.max_payload_bytes;
				if (control_bytes > profile.expectation.limits.max_control_bytes ||
					payload_bytes > profile.expectation.limits.max_payload_bytes ||
					payload_bytes > profile_payload_limit ||
					payload_bytes >
						std::numeric_limits<std::size_t>::max() - wire_header_bytes - control_bytes)
					return cxxlens::sdk::unexpected(runtime_error("provider.oversized-frame",
																  profile.expectation.task.task_id,
																  "host-input"));
				std::vector<std::byte> encoded(wire_header_bytes + control_bytes +
											   static_cast<std::size_t>(payload_bytes));
				std::ranges::copy(header, encoded.begin());
				if (auto complete =
						read_exact(std::span{encoded}.subspan(wire_header_bytes), false);
					!complete)
					return cxxlens::sdk::unexpected(std::move(complete.error()));
				auto decoded = decode_frame(encoded, profile.expectation.limits);
				if (!decoded)
					return cxxlens::sdk::unexpected(std::move(decoded.error()));
				if (auto accepted = state.consume(*decoded); !accepted)
					return cxxlens::sdk::unexpected(std::move(accepted.error()));
			}
			auto sealed = state.finish();
			if (!sealed)
				return cxxlens::sdk::unexpected(std::move(sealed.error()));
			auto digest = ordered_input_chunk_set_digest(sealed->task.task_id,
														 sealed->task.task_input_digest,
														 sealed->ordered_chunk_digests);
			if (!digest)
				return cxxlens::sdk::unexpected(std::move(digest.error()));
			return sealed_host_input{std::move(sealed->task),
									 sealed->credit,
									 profile.expectation.limits.protocol_major,
									 profile.expectation.limits.maximum_minor,
									 sealed->total_bytes,
									 sealed->chunk_bytes,
									 std::move(sealed->ordered_chunk_digests),
									 std::move(*digest)};
		}

		sealed_provider_batch::sealed_provider_batch(std::string task_id,
													 std::string descriptor_id,
													 std::string descriptor_digest,
													 std::string dependency_group_id,
													 std::string atomic_output_group_id,
													 std::string batch_id,
													 std::string batch_digest,
													 std::vector<std::string> ordered_chunk_digests,
													 std::vector<detached_row> rows)
			: task_id_{std::move(task_id)}, descriptor_id_{std::move(descriptor_id)},
			  descriptor_digest_{std::move(descriptor_digest)},
			  dependency_group_id_{std::move(dependency_group_id)},
			  atomic_output_group_id_{std::move(atomic_output_group_id)},
			  batch_id_{std::move(batch_id)}, batch_digest_{std::move(batch_digest)},
			  ordered_chunk_digests_{std::move(ordered_chunk_digests)}, rows_{std::move(rows)}
		{
		}

		std::string_view sealed_provider_batch::task_id() const noexcept
		{
			return task_id_;
		}
		std::string_view sealed_provider_batch::descriptor_id() const noexcept
		{
			return descriptor_id_;
		}
		std::string_view sealed_provider_batch::descriptor_digest() const noexcept
		{
			return descriptor_digest_;
		}
		std::string_view sealed_provider_batch::dependency_group_id() const noexcept
		{
			return dependency_group_id_;
		}
		std::string_view sealed_provider_batch::atomic_output_group_id() const noexcept
		{
			return atomic_output_group_id_;
		}
		std::string_view sealed_provider_batch::batch_id() const noexcept
		{
			return batch_id_;
		}
		std::string_view sealed_provider_batch::batch_digest() const noexcept
		{
			return batch_digest_;
		}
		std::span<const std::string> sealed_provider_batch::ordered_chunk_digests() const noexcept
		{
			return ordered_chunk_digests_;
		}
		std::span<const detached_row> sealed_provider_batch::rows() const noexcept
		{
			return rows_;
		}

		sealed_provider_transcript::sealed_provider_transcript(
			std::vector<sealed_provider_batch> batches,
			std::vector<coverage_unit> coverage,
			std::vector<unresolved_item> unresolved,
			std::vector<evidence_item> evidence)
			: batches_{std::move(batches)}, coverage_{std::move(coverage)},
			  unresolved_{std::move(unresolved)}, evidence_{std::move(evidence)}
		{
		}

		std::span<const sealed_provider_batch> sealed_provider_transcript::batches() const noexcept
		{
			return batches_;
		}
		std::span<const coverage_unit> sealed_provider_transcript::coverage() const noexcept
		{
			return coverage_;
		}
		std::span<const unresolved_item> sealed_provider_transcript::unresolved() const noexcept
		{
			return unresolved_;
		}
		std::span<const evidence_item> sealed_provider_transcript::evidence() const noexcept
		{
			return evidence_;
		}

		const std::optional<sealed_provider_transcript>&
		transcript_validation_result::sealed() const noexcept
		{
			return sealed_;
		}
		std::optional<sealed_provider_transcript>
		transcript_validation_result::take_sealed() && noexcept
		{
			return std::move(sealed_);
		}
		const std::optional<error>& transcript_validation_result::sealing_error() const noexcept
		{
			return sealing_error_;
		}
		std::optional<error> transcript_validation_result::take_sealing_error() && noexcept
		{
			return std::move(sealing_error_);
		}

		result<transcript_validation_result>
		validate_provider_transcript(const transcript_validation_request& request,
									 const std::span<const frame> frames,
									 const protocol_limits session_limits)
		{
			const auto fail = [](std::string code, std::string field, std::string detail = {})
			{
				return result<transcript_validation_result>{cxxlens::sdk::unexpected(
					runtime_error(std::move(code), std::move(field), std::move(detail)))};
			};
			std::uint64_t consumed_bytes{};
			std::uint64_t logical_output_bytes{};
			for (const auto& value : frames)
			{
				if (request.budget != nullptr && detail::counts_toward_output_budget(value.type))
				{
					const auto control_bytes = static_cast<std::uint64_t>(value.control.size());
					const auto payload_bytes = static_cast<std::uint64_t>(value.payload.size());
					if (control_bytes > request.budget->output_bytes - logical_output_bytes ||
						payload_bytes >
							request.budget->output_bytes - logical_output_bytes - control_bytes)
						return fail("provider.output-limit", request.task_id, "output_bytes");
					logical_output_bytes += control_bytes + payload_bytes;
				}
				auto encoded = encode_frame(value, session_limits);
				if (!encoded || consumed_bytes > request.output_credit.bytes ||
					encoded->size() > request.output_credit.bytes - consumed_bytes)
					return fail("provider.credit-exceeded", request.task_id, "bytes");
				consumed_bytes += encoded->size();
			}
			if (frames.size() > request.output_credit.frames)
				return fail("provider.credit-exceeded", request.task_id, "frames");
			if (request.expected_provider != nullptr)
			{
				if (auto valid = request.expected_provider->validate(); !valid)
					return cxxlens::sdk::unexpected(std::move(valid.error()));
				if (request.provider_manifest == nullptr)
					return fail(
						"provider.binary-identity-mismatch", request.task_id, "manifest-authority");
				const auto& identity = *request.expected_provider;
				const auto& provider = *request.provider_manifest;
				auto required_features = provider.protocol.required_features;
				std::ranges::sort(required_features);
				auto offered_relations = provider.offered_relations;
				std::ranges::sort(offered_relations);
				const auto identity_mismatch = [&](const std::string_view field)
				{
					return fail("provider.binary-identity-mismatch",
								request.task_id,
								std::string{"expected-provider-"} + std::string{field});
				};
				if (identity.provider_id != request.provider_id ||
					identity.provider_version != request.provider_version ||
					identity.provider_id != provider.provider_id ||
					identity.provider_version != provider.provider_version)
					return identity_mismatch("id-version");
				if (identity.provider_binary_digest != provider.provider_binary_digest ||
					identity.provider_semantic_contract_digest !=
						provider.provider_semantic_contract_digest)
					return identity_mismatch("digests");
				if (identity.protocol_major != session_limits.protocol_major ||
					identity.protocol_minor != session_limits.maximum_minor)
					return identity_mismatch("protocol");
				if (identity.required_features != required_features)
					return identity_mismatch("features");
				if (identity.offered_relations != offered_relations ||
					std::ranges::any_of(request.output_descriptors,
										[&](const relation_descriptor& descriptor)
										{
											return !offered_relation(provider, descriptor.id);
										}))
					return identity_mismatch("offers");
			}

			std::uint64_t expected_sequence{};
			bool hello_seen{!request.require_handshake};
			bool schema_seen{!request.require_handshake};
			bool accepted{};
			bool coverage_seen{};
			bool unresolved_seen{};
			bool progress_seen{};
			bool terminal_seen{};
			std::uint64_t output_rows{};
			std::uint64_t diagnostics{};
			transcript_validation_result terminal;
			std::vector<sealed_provider_batch> sealed_batches;
			std::vector<coverage_unit> sealed_coverage;
			std::vector<unresolved_item> sealed_unresolved;
			std::vector<evidence_item> sealed_evidence;
			std::set<std::string, std::less<>> batches;
			struct open_batch
			{
				const relation_descriptor* descriptor{};
				std::string dependency_group_id;
				std::string atomic_output_group_id;
				std::string id;
				std::vector<batch_column_summary> columns;
				std::vector<std::string> ordered_chunk_digests;
				std::map<std::string, std::uint64_t, std::less<>> next_row_offsets;
				std::map<std::string, std::uint64_t, std::less<>> next_chunk_indexes;
				std::map<std::string, std::vector<detached_cell>, std::less<>> column_cells;
				std::optional<std::uint64_t> cycle_row_offset;
				std::optional<std::uint32_t> cycle_row_count;
			};
			std::optional<open_batch> batch;
			const auto expected_hello = request.provider_manifest == nullptr
				? std::string{}
				: request.provider_manifest->canonical_json();
			for (std::size_t index = 0U; index < frames.size(); ++index)
			{
				const auto& value = frames[index];
				if (value.stream_id != 1U || value.sequence != expected_sequence++)
					return fail("provider.protocol-state-invalid", request.task_id, "sequence");
				const auto optional_extension =
					(value.flags & static_cast<std::uint16_t>(frame_flag::optional_extension)) !=
					0U;
				const auto end_of_stream =
					(value.flags & static_cast<std::uint16_t>(frame_flag::end_of_stream)) != 0U;
				if (end_of_stream &&
					(index + 1U != frames.size() ||
					 (value.type != message_type::task_complete &&
					  value.type != message_type::task_failed)))
					return fail(
						"provider.protocol-state-invalid", request.task_id, "end-of-stream");
				if (terminal_seen ||
					(index + 1U == frames.size() && value.type != message_type::task_complete &&
					 value.type != message_type::task_failed))
					return fail(
						"provider.protocol-state-invalid", request.task_id, "terminal-order");
				if (optional_extension)
					continue;
				std::optional<std::string> control;
				if (value.type == message_type::hello)
				{
					auto decoded = decode_control_text(value.control);
					if (!decoded)
						return fail("provider.protocol-state-invalid", request.task_id, "control");
					if (decoded->contains('\0'))
						return fail(
							"provider.protocol-state-invalid", request.task_id, "control-nul");
					control = std::move(*decoded);
				}
				switch (value.type)
				{
					case message_type::hello:
						if (!request.require_handshake || index != 0U || hello_seen ||
							*control != expected_hello || !value.payload.empty())
							return fail(
								"provider.binary-identity-mismatch", request.task_id, "hello");
						hello_seen = true;
						break;
					case message_type::schema_negotiate:
					{
						auto metadata = decode_schema_negotiate_metadata(value.control);
						if (!hello_seen || schema_seen || accepted || !metadata ||
							metadata->protocol_schema != "cxxlens.provider-protocol.v1" ||
							metadata->protocol_minor != session_limits.maximum_minor ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "schema");
						schema_seen = true;
						break;
					}
					case message_type::task_accepted:
					{
						auto metadata = decode_task_accepted_metadata(value.control);
						if (!metadata)
							return fail("provider.protocol-state-invalid",
										request.task_id,
										"control-metadata");
						const auto& expected_id = request.expected_provider == nullptr
							? request.provider_id
							: request.expected_provider->provider_id;
						const auto expected_version = request.expected_provider == nullptr
							? request.provider_version.string()
							: request.expected_provider->provider_version.string();
						if (!schema_seen || accepted || metadata->provider_id != expected_id ||
							metadata->provider_version != expected_version ||
							metadata->task_id != request.task_id || !value.payload.empty())
							return fail(
								"provider.task-binding-mismatch", request.task_id, "accepted");
						accepted = true;
						break;
					}
					case message_type::batch_begin:
					{
						auto metadata = decode_batch_begin_metadata(value.control);
						if (!accepted || batch || !metadata ||
							metadata->task_id != request.task_id ||
							metadata->descriptor_id.empty() ||
							metadata->dependency_group_id.empty() ||
							metadata->atomic_output_group_id.empty() ||
							metadata->batch_id.empty() ||
							!protocol_digest(metadata->descriptor_digest) ||
							!value.payload.empty() || !batches.insert(metadata->batch_id).second)
							return fail("provider.batch-invalid", request.task_id, "begin");
						if (request.provider_manifest != nullptr &&
							!offered_relation(*request.provider_manifest, metadata->descriptor_id))
							return fail(
								"provider.relation-incompatible", metadata->descriptor_id, "offer");
						const auto* descriptor =
							output_descriptor(request.output_descriptors, metadata->descriptor_id);
						if (descriptor == nullptr ||
							descriptor->descriptor_digest != metadata->descriptor_digest)
							return fail("provider.relation-incompatible",
										metadata->descriptor_id,
										"descriptor-digest");
						open_batch opened{descriptor,
										  std::move(metadata->dependency_group_id),
										  std::move(metadata->atomic_output_group_id),
										  std::move(metadata->batch_id),
										  {},
										  {},
										  {},
										  {},
										  {},
										  {},
										  {}};
						for (const auto& column : descriptor->columns)
						{
							opened.columns.push_back({column.id, 0U, 0U});
							opened.next_row_offsets.emplace(column.id, 0U);
							opened.next_chunk_indexes.emplace(column.id, 0U);
							opened.column_cells.emplace(column.id, std::vector<detached_cell>{});
						}
						batch = std::move(opened);
						break;
					}
					case message_type::column_chunk:
					{
						if (!batch || value.payload.empty() || batch->descriptor->columns.empty())
							return fail("provider.batch-invalid", request.task_id, "column");
						auto chunk =
							decode_column_chunk(value.control, value.payload, *batch->descriptor);
						if (!chunk)
							return fail(
								"provider.batch-invalid", request.task_id, chunk.error().detail);
						const auto expected_column_index =
							batch->ordered_chunk_digests.size() % batch->descriptor->columns.size();
						const auto& expected_column =
							batch->descriptor->columns[expected_column_index];
						const auto summary = std::ranges::find(
							batch->columns, chunk->column_id, &batch_column_summary::column_id);
						const auto starts_cycle = expected_column_index == 0U;
						if (starts_cycle)
						{
							batch->cycle_row_offset = chunk->row_offset;
							batch->cycle_row_count = chunk->row_count;
						}
						if (chunk->task_id != request.task_id ||
							chunk->dependency_group_id != batch->dependency_group_id ||
							chunk->atomic_output_group_id != batch->atomic_output_group_id ||
							chunk->batch_id != batch->id ||
							chunk->descriptor_id != batch->descriptor->id ||
							chunk->descriptor_digest != batch->descriptor->descriptor_digest ||
							chunk->column_id != expected_column.id ||
							summary == batch->columns.end() ||
							chunk->row_offset != batch->next_row_offsets.at(chunk->column_id) ||
							chunk->chunk_index != batch->next_chunk_indexes.at(chunk->column_id) ||
							!batch->cycle_row_offset || !batch->cycle_row_count ||
							chunk->row_offset != *batch->cycle_row_offset ||
							chunk->row_count != *batch->cycle_row_count ||
							chunk->cells.size() != chunk->row_count ||
							chunk->row_count > std::numeric_limits<std::uint64_t>::max() -
									batch->next_row_offsets.at(chunk->column_id))
							return fail("provider.batch-invalid", request.task_id, "chunk-binding");
						summary->payload_bytes += value.payload.size();
						++summary->chunk_count;
						batch->next_row_offsets.at(chunk->column_id) += chunk->row_count;
						++batch->next_chunk_indexes.at(chunk->column_id);
						auto& cells = batch->column_cells.at(chunk->column_id);
						cells.insert(cells.end(),
									 std::make_move_iterator(chunk->cells.begin()),
									 std::make_move_iterator(chunk->cells.end()));
						batch->ordered_chunk_digests.push_back(std::move(chunk->chunk_digest));
						if (expected_column_index + 1U == batch->descriptor->columns.size())
						{
							batch->cycle_row_offset.reset();
							batch->cycle_row_count.reset();
						}
						break;
					}
					case message_type::batch_end:
					{
						if (!batch)
							return fail(
								"provider.batch-invalid", request.task_id, "end-without-batch");
						auto batch_terminal =
							decode_columnar_batch_end(value.control, value.payload);
						if (!batch_terminal)
							return fail("provider.batch-invalid",
										request.task_id,
										batch_terminal.error().detail);
						const bool all_rows_match =
							std::ranges::all_of(batch->next_row_offsets,
												[&](const auto& item)
												{
													return item.second == batch_terminal->row_count;
												});
						if (batch_terminal->task_id != request.task_id ||
							batch_terminal->dependency_group_id != batch->dependency_group_id ||
							batch_terminal->atomic_output_group_id !=
								batch->atomic_output_group_id ||
							batch_terminal->batch_id != batch->id ||
							batch_terminal->descriptor_id != batch->descriptor->id ||
							batch_terminal->descriptor_digest !=
								batch->descriptor->descriptor_digest ||
							batch_terminal->columns != batch->columns ||
							batch_terminal->ordered_chunk_digests != batch->ordered_chunk_digests ||
							batch->cycle_row_offset || batch->cycle_row_count || !all_rows_match)
							return fail("provider.batch-invalid", request.task_id, "end");
						if (request.budget != nullptr &&
							batch_terminal->row_count > request.budget->rows - output_rows)
							return fail("provider.output-limit", request.task_id, "rows");
						output_rows += batch_terminal->row_count;
						std::vector<detached_row> rows;
						rows.reserve(batch_terminal->row_count);
						bool batch_sealed{true};
						for (std::uint64_t row_index = 0U; row_index < batch_terminal->row_count;
							 ++row_index)
						{
							detached_row row;
							row.descriptor_id = batch->descriptor->id;
							for (const auto& column : batch->descriptor->columns)
							{
								auto& cells = batch->column_cells.at(column.id);
								if (cells.size() != batch_terminal->row_count)
									return fail("provider.batch-invalid",
												request.task_id,
												"column-row-count");
								row.cells.emplace(column.id, std::move(cells.at(row_index)));
							}
							if (auto valid = validate_row(*batch->descriptor, row); !valid)
							{
								if (!terminal.sealing_error_)
									terminal.sealing_error_ = std::move(valid.error());
								batch_sealed = false;
							}
							else if (batch->descriptor->domain_identity.result_column)
							{
								auto identity = validate_domain_identity(*batch->descriptor, row);
								if (!identity)
								{
									if (!terminal.sealing_error_)
										terminal.sealing_error_ = std::move(identity.error());
									batch_sealed = false;
								}
							}
							rows.push_back(std::move(row));
						}
						if (batch_sealed)
							sealed_batches.push_back(
								sealed_provider_batch{request.task_id,
													  batch->descriptor->id,
													  batch->descriptor->descriptor_digest,
													  batch->dependency_group_id,
													  batch->atomic_output_group_id,
													  batch->id,
													  batch_terminal->batch_digest,
													  batch->ordered_chunk_digests,
													  std::move(rows)});
						batch.reset();
						break;
					}
					case message_type::coverage_chunk:
					{
						auto records = decode_coverage_metadata(value.control);
						if (!accepted || batch || coverage_seen || !records ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "coverage");
						coverage_seen = true;
						bool task_covered{};
						std::set<std::pair<std::string, std::string>> seen;
						for (const auto& record : *records)
						{
							static const std::set<std::string_view> states{
								"covered", "excluded", "failed", "not_applicable", "unresolved"};
							if (record.kind.empty() || record.id.empty() ||
								!states.contains(record.state) ||
								!seen.emplace(record.kind, record.id).second)
								return fail(
									"provider.coverage-incomplete", request.task_id, "coverage");
							task_covered = task_covered ||
								(record.kind == "task" && record.id == request.task_id &&
								 record.state == "covered");
						}
						if (!task_covered)
							return fail("provider.coverage-incomplete", request.task_id, "task");
						sealed_coverage = std::move(*records);
						break;
					}
					case message_type::unresolved_chunk:
					{
						auto records = decode_unresolved_metadata(value.control);
						if (!accepted || batch || unresolved_seen || !records ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "side-channel");
						unresolved_seen = true;
						if (request.budget != nullptr &&
							records->size() > request.budget->diagnostics - diagnostics)
							return fail("provider.output-limit", request.task_id, "diagnostics");
						diagnostics += records->size();
						for (const auto& record : *records)
							if (!namespaced(record.code) || record.code.contains('\0') ||
								record.subject.empty())
								return fail("provider.protocol-state-invalid",
											request.task_id,
											"side-channel-value");
						sealed_unresolved = std::move(*records);
						break;
					}
					case message_type::progress:
					{
						auto records = decode_evidence_metadata(value.control);
						if (!accepted || batch || progress_seen || !records ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "side-channel");
						progress_seen = true;
						for (const auto& record : *records)
							if (!namespaced(record.kind) || record.kind.contains('\0') ||
								record.subject.empty() || record.producer.empty())
								return fail("provider.protocol-state-invalid",
											request.task_id,
											"side-channel-value");
						sealed_evidence = std::move(*records);
						break;
					}
					case message_type::task_complete:
					{
						auto metadata = decode_task_complete_metadata(value.control);
						if (!accepted || batch || !coverage_seen || !unresolved_seen ||
							!progress_seen || !metadata || metadata->task_id != request.task_id ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "complete");
						terminal.kind = transcript_terminal_kind::complete;
						terminal.reason = "provider.success";
						terminal_seen = true;
						break;
					}
					case message_type::task_failed:
					{
						auto metadata = decode_task_failed_metadata(value.control);
						if (!metadata || metadata->error_code.contains('\0'))
							return fail("provider.protocol-state-invalid",
										request.task_id,
										"control-metadata");
						if (!schema_seen || !namespaced(metadata->error_code) ||
							metadata->task_id != request.task_id || !value.payload.empty())
							return fail(
								"provider.task-binding-mismatch", request.task_id, "failed");
						if (!allowed_failure_terminal(metadata->error_code))
							return fail(
								"provider.schema-invalid", request.task_id, "failure-reason");
						terminal.kind = transcript_terminal_kind::failed;
						terminal.reason = std::move(metadata->error_code);
						terminal_seen = true;
						break;
					}
					case message_type::hello_ack:
					case message_type::open_task:
					case message_type::input_descriptor:
					case message_type::input_chunk:
					case message_type::credit:
					case message_type::batch_ack:
					case message_type::batch_reject:
					case message_type::cancel:
						return fail(
							"provider.protocol-state-invalid", request.task_id, "direction");
					case message_type::closure_candidate:
					case message_type::resume:
					case message_type::close:
						return fail(
							"provider.protocol-state-invalid", request.task_id, "unsupported");
				}
			}
			if (!hello_seen || !schema_seen || !terminal_seen)
				return fail("provider.truncated-stream", request.task_id, "state");
			if (terminal.kind == transcript_terminal_kind::complete && !terminal.sealing_error_)
				terminal.sealed_ = sealed_provider_transcript{std::move(sealed_batches),
															  std::move(sealed_coverage),
															  std::move(sealed_unresolved),
															  std::move(sealed_evidence)};
			return terminal;
		}
	} // namespace detail

	namespace
	{

#if !defined(CXXLENS_PROVIDER_RUNTIME_INTERNAL_ONLY)
		[[nodiscard]] std::string transcript_projection(const std::span<const frame> frames)
		{
			std::ostringstream output;
			for (const auto& value : frames)
				output << value.protocol_major << '|' << value.protocol_minor << '|' << value.flags
					   << '|' << static_cast<std::uint16_t>(value.type) << '|' << value.stream_id
					   << '|' << value.sequence << '|' << content_digest(value.control) << '|'
					   << content_digest(value.payload) << '\n';
			return output.str();
		}
#endif

		[[nodiscard]] std::string terminal_for_status(const process_status status)
		{
			switch (status)
			{
				case process_status::timed_out:
					return "provider.timeout";
				case process_status::cancelled:
					return "provider.cancelled";
				case process_status::output_limit:
					return "provider.output-limit";
				case process_status::crashed:
					return "provider.crash";
				case process_status::unavailable:
				case process_status::launch_failed:
					return "provider.runtime-unavailable";
				case process_status::exited:
					return "provider.crash";
			}
			return "provider.runtime-unavailable";
		}

		[[nodiscard]] bool sandbox_satisfies(const sandbox_assurance achieved,
											 const sandbox_assurance required) noexcept
		{
			return is_valid(achieved) && is_valid(required) &&
				static_cast<std::uint8_t>(achieved) >= static_cast<std::uint8_t>(required);
		}

		[[nodiscard]] std::optional<sandbox_assurance>
		parse_sandbox_assurance(const std::string_view value) noexcept
		{
			if (value == "none")
				return sandbox_assurance::none;
			if (value == "best_effort")
				return sandbox_assurance::best_effort;
			if (value == "enforced")
				return sandbox_assurance::enforced;
			if (value == "certified")
				return sandbox_assurance::certified;
			return std::nullopt;
		}

		[[nodiscard]] result<sandbox_requirement>
		effective_sandbox(const process_task_request& request)
		{
			const auto& authority = request.selection.authority_request().sandbox;
			if (auto valid = authority.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			if (auto valid = request.sandbox.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			if (request.sandbox.policy_digest != authority.policy_digest)
				return cxxlens::sdk::unexpected(
					runtime_error("security.sandbox-policy-mismatch", "selection"));
			const auto manifest_minimum = parse_sandbox_assurance(
				request.selection.selected_candidate().description.sandbox_minimum);
			if (!manifest_minimum)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.manifest-invalid", "sandbox_minimum"));
			auto minimum = authority.minimum;
			if (sandbox_satisfies(request.sandbox.minimum, minimum))
				minimum = request.sandbox.minimum;
			if (sandbox_satisfies(*manifest_minimum, minimum))
				minimum = *manifest_minimum;
			return sandbox_requirement{minimum, authority.policy_digest};
		}

		[[nodiscard]] detail::provider_process_validation_outcome transport_failure_outcome(
			const process_task_request& request, process_output output, std::string terminal)
		{
			detail::provider_process_validation_outcome report;
			report.terminal = stable_terminal_reason(terminal)
				? std::move(terminal)
				: std::string{"provider.runtime-unavailable"};
			report.provider = request.selection.selected_candidate().description;
			report.task_input_digest = request.task_input_digest;
			report.normalized_invocation_digest = request.normalized_invocation_digest;
			report.toolchain_digest = request.toolchain_digest;
			report.environment_digest = request.environment_digest;
			report.measured_executable_digest = std::move(output.measured_executable_digest);
			report.sandbox = std::move(output.sandbox);
			report.exit_code = output.exit_code;
			report.termination_signal = output.termination_signal;
			if (!output.standard_error.empty())
				report.diagnostics.push_back(
					{"provider.worker-stderr", request.task_id, std::move(output.standard_error)});
			return report;
		}
	} // namespace

#if !defined(CXXLENS_PROVIDER_RUNTIME_INTERNAL_ONLY)
	bool process_execution_report::succeeded() const noexcept
	{
		return validated_success_ && terminal == "provider.success" && !frames.empty() &&
			frames.back().type == message_type::task_complete;
	}

	std::string process_execution_report::canonical_form() const
	{
		std::ostringstream output;
		output << "{\"diagnostics\":[";
		for (std::size_t index = 0U; index < diagnostics.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& diagnostic = diagnostics[index];
			output << "{\"code\":" << json_string(diagnostic.code)
				   << ",\"detail\":" << json_string(diagnostic.detail)
				   << ",\"subject\":" << json_string(diagnostic.subject) << '}';
		}
		const auto transcript = transcript_projection(frames);
		output << R"(],"frames":{"count":)" << frames.size() << R"(,"last_sequence":)";
		if (frames.empty())
			output << "null";
		else
			output << frames.back().sequence;
		output << R"(,"transcript_digest":)"
			   << json_string(
					  *cxxlens::sdk::semantic_digest("cxxlens.provider-transcript.v1", transcript))
			   << R"(},"input_binding":{"environment":)" << json_string(environment_digest)
			   << R"(,"invocation":)" << json_string(normalized_invocation_digest) << R"(,"task":)"
			   << json_string(task_input_digest) << R"(,"toolchain":)"
			   << json_string(toolchain_digest) << R"(},"measured_executable_digest":)";
		if (measured_executable_digest.empty())
			output << "null";
		else
			output << json_string(measured_executable_digest);
		output << R"(,"provider":{"binary_digest":)" << json_string(provider.provider_binary_digest)
			   << ",\"id\":" << json_string(provider.provider_id)
			   << ",\"semantic_contract_digest\":"
			   << json_string(provider.provider_semantic_contract_digest)
			   << ",\"version\":" << json_string(provider.provider_version.string())
			   << "},\"sandbox\":" << sandbox.canonical_form()
			   << R"(,"schema":"cxxlens.provider-execution-report.v1","semantic_digest":)"
			   << json_string(semantic_digest()) << ",\"terminal\":" << json_string(terminal)
			   << '}';
		return output.str();
	}

	std::string process_execution_report::semantic_digest() const
	{
		std::ostringstream projection;
		projection << terminal << '|' << provider.provider_id << '|'
				   << provider.provider_version.string() << '|' << provider.provider_binary_digest
				   << '|' << provider.provider_semantic_contract_digest << '|' << task_input_digest
				   << '|' << normalized_invocation_digest << '|' << toolchain_digest << '|'
				   << environment_digest << '|' << measured_executable_digest << '|'
				   << sandbox.canonical_form() << '|' << transcript_projection(frames);
		for (const auto& diagnostic : diagnostics)
			projection << diagnostic.code << '|' << diagnostic.subject << '|' << diagnostic.detail
					   << '\n';
		return *cxxlens::sdk::semantic_digest("cxxlens.provider-execution-report.v1",
											  projection.str());
	}

	process_provider_runtime::process_provider_runtime(const provider_process_port& processes)
		: processes_{&processes}
	{
	}
#endif

	namespace
	{
		class vector_host_input final : public detail::replayable_host_input
		{
		  public:
			explicit vector_host_input(const std::span<const std::byte> bytes) : bytes_{bytes} {}
			result<std::uint64_t> size() const override
			{
				return bytes_.size();
			}
			result<std::size_t> read_at(const std::uint64_t offset,
										const std::span<std::byte> output) const override
			{
				if (offset > bytes_.size())
					return cxxlens::sdk::unexpected(
						runtime_error("provider.task-invalid", "payload", "read-offset"));
				const auto count = std::min<std::size_t>(output.size(), bytes_.size() - offset);
				std::ranges::copy(bytes_.subspan(static_cast<std::size_t>(offset), count),
								  output.begin());
				return count;
			}

		  private:
			std::span<const std::byte> bytes_;
		};

		class vector_frame_output final : public frame_sink
		{
		  public:
			result<void> write(const std::span<const std::byte> bytes) override
			{
				bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
				return {};
			}
			std::vector<std::byte> bytes_;
		};

		struct prepared_provider_process
		{
			protocol_limits session_limits;
			sandbox_requirement sandbox;
			detail::expected_provider_identity provider_identity;
			detail::host_input_profile input_profile;
			process_invocation invocation;
		};

		[[nodiscard]] result<prepared_provider_process>
		prepare_provider_process(const process_task_request& request)
		{
			if (auto valid = request.selection.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			if (request.task_id.empty() || request.task_id.contains('\0') ||
				request.selection.selected_candidate().executable_argv.empty() ||
				request.selection.selected_candidate().executable_argv.front().empty() ||
				!canonical_digest(request.task_input_digest) ||
				!canonical_digest(request.normalized_invocation_digest) ||
				!canonical_digest(request.toolchain_digest) ||
				!canonical_digest(request.environment_digest))
				return cxxlens::sdk::unexpected(
					runtime_error("provider.task-invalid", request.task_id));
			const auto& selected = request.selection.selected_candidate();
			const auto& provider = selected.description;
			if (auto valid = provider.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			static const std::set<std::string, std::less<>> supported_features{
				"credit-backpressure", "task-input-chunks-v1"};
			if (std::ranges::any_of(provider.protocol.required_features,
									[&](const std::string& feature)
									{
										return !supported_features.contains(feature);
									}))
				return cxxlens::sdk::unexpected(
					runtime_error("provider.required-feature-missing", "protocol"));
			if (request.output_descriptors.empty())
				return cxxlens::sdk::unexpected(
					runtime_error("provider.task-invalid", "output_descriptors"));
			std::set<std::string, std::less<>> output_descriptor_ids;
			for (const auto& descriptor : request.output_descriptors)
			{
				if (auto valid = descriptor.validate(); !valid)
					return cxxlens::sdk::unexpected(std::move(valid.error()));
				if (!output_descriptor_ids.insert(descriptor.id).second ||
					!offered_relation(provider, descriptor.id))
					return cxxlens::sdk::unexpected(runtime_error(
						"provider.relation-incompatible", descriptor.id, "output-descriptor"));
			}
			if (auto valid = request.sandbox.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			auto sandbox = effective_sandbox(request);
			if (!sandbox)
				return cxxlens::sdk::unexpected(std::move(sandbox.error()));
			if (auto valid = request.budget.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			if (request.output_credit.bytes == 0U || request.output_credit.frames == 0U)
				return cxxlens::sdk::unexpected(runtime_error("provider.task-invalid", "budget"));
			auto session_limits = negotiated_limits(request);
			if (!session_limits)
				return cxxlens::sdk::unexpected(std::move(session_limits.error()));

			auto required_features = provider.protocol.required_features;
			std::ranges::sort(required_features);
			auto offered_relations = provider.offered_relations;
			std::ranges::sort(offered_relations);
			detail::expected_provider_identity identity{
				provider.provider_id,
				provider.provider_version,
				provider.provider_binary_digest,
				provider.provider_semantic_contract_digest,
				session_limits->protocol_major,
				session_limits->maximum_minor,
				std::move(required_features),
				sandbox->policy_digest,
				std::move(offered_relations),
			};
			if (auto valid = identity.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			detail::host_input_profile profile{
				{provider.canonical_json(),
				 {request.task_id,
				  request.task_input_digest,
				  request.normalized_invocation_digest,
				  request.toolchain_digest,
				  request.environment_digest},
				 *session_limits},
				session_limits->maximum_minor == 1U,
			};
			process_invocation invocation;
			invocation.argv = selected.executable_argv;
			invocation.environment = {
				{"CXXLENS_PROVIDER_ID", provider.provider_id},
				{"CXXLENS_PROVIDER_MANIFEST", provider.canonical_json()},
				{"CXXLENS_PROVIDER_BINARY_DIGEST", provider.provider_binary_digest},
				{"CXXLENS_PROVIDER_SEMANTIC_CONTRACT_DIGEST",
				 provider.provider_semantic_contract_digest},
				{"CXXLENS_PROVIDER_TASK_ID", request.task_id},
				{"CXXLENS_PROVIDER_TASK_INPUT_DIGEST", request.task_input_digest},
				{"CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST",
				 request.normalized_invocation_digest},
				{"CXXLENS_PROVIDER_TOOLCHAIN_DIGEST", request.toolchain_digest},
				{"CXXLENS_PROVIDER_ENVIRONMENT_DIGEST", request.environment_digest},
				{"CXXLENS_PROVIDER_PROTOCOL_MAJOR", std::to_string(session_limits->protocol_major)},
				{"CXXLENS_PROVIDER_PROTOCOL_MINOR", std::to_string(session_limits->maximum_minor)},
			};
			invocation.budget = request.budget;
			invocation.sandbox = *sandbox;
			invocation.expected_binary_digest = provider.provider_binary_digest;
			return prepared_provider_process{*session_limits,
											 *std::move(sandbox),
											 std::move(identity),
											 std::move(profile),
											 std::move(invocation)};
		}

		[[nodiscard]] result<detail::provider_process_validation_outcome>
		finish_provider_process(const process_task_request& request,
								prepared_provider_process prepared,
								detail::sealed_host_input input_seal,
								process_output output)
		{
			if (auto valid = output.sandbox.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			if (output.sandbox.policy_digest != prepared.sandbox.policy_digest)
				return transport_failure_outcome(
					request, std::move(output), "security.sandbox-policy-mismatch");
			auto applied_policy = resolve_sandbox_policy(output.sandbox.policy_digest);
			const auto& selected_binary_digest = prepared.provider_identity.provider_binary_digest;
			const auto evidence_binary_digest = canonical_digest(output.measured_executable_digest)
				? std::string_view{output.measured_executable_digest}
				: std::string_view{selected_binary_digest};
			auto evidence = applied_policy
				? sandbox_evidence_digest(*applied_policy,
										  request.budget,
										  output.sandbox.achieved,
										  output.sandbox.mechanisms,
										  evidence_binary_digest)
				: result<std::string>{unexpected(
					  runtime_error("security.sandbox-policy-mismatch", "unknown-policy"))};
			if (!evidence || output.sandbox.evidence_digest != *evidence)
				return transport_failure_outcome(
					request, std::move(output), "security.sandbox-policy-mismatch");
			if (sandbox_satisfies(output.sandbox.achieved, sandbox_assurance::enforced) &&
				output.measured_executable_digest != selected_binary_digest)
				return transport_failure_outcome(
					request, std::move(output), "provider.binary-identity-mismatch");
			if (sandbox_satisfies(output.sandbox.achieved, sandbox_assurance::enforced))
			{
				auto actual_mechanisms = output.sandbox.mechanisms;
				std::ranges::sort(actual_mechanisms);
				if (!applied_policy || actual_mechanisms != applied_policy->mechanisms)
					return transport_failure_outcome(
						request, std::move(output), "security.sandbox-policy-mismatch");
			}
			if (output.status != process_status::exited)
			{
				const auto terminal = output.failure_code.empty()
					? terminal_for_status(output.status)
					: output.failure_code;
				return transport_failure_outcome(request, std::move(output), terminal);
			}
			if (!sandbox_satisfies(output.sandbox.achieved, prepared.sandbox.minimum))
				return transport_failure_outcome(
					request, std::move(output), "security.sandbox-insufficient");
			if (output.exit_code != 0)
				return transport_failure_outcome(request, std::move(output), "provider.crash");

			const auto raw_stdout_byte_count =
				static_cast<std::uint64_t>(output.standard_output.size());
			auto raw_stdout_sha256 = content_digest(output.standard_output);
			auto frames = decode_frame_stream(output.standard_output, prepared.session_limits);
			if (!frames)
			{
				auto report =
					transport_failure_outcome(request, std::move(output), frames.error().code);
				report.diagnostics.push_back(
					{frames.error().code, request.task_id, frames.error().field});
				return report;
			}
			const auto& selected_manifest = request.selection.selected_candidate().description;
			const detail::transcript_validation_request validation{
				request.task_id,
				selected_manifest.provider_id,
				selected_manifest.provider_version,
				&selected_manifest,
				request.output_descriptors,
				request.output_credit,
				&request.budget,
				true,
				&prepared.provider_identity,
			};
			auto terminal =
				detail::validate_provider_transcript(validation, *frames, prepared.session_limits);
			if (!terminal)
			{
				auto validation_error = std::move(terminal.error());
				auto report =
					transport_failure_outcome(request, std::move(output), validation_error.code);
				report.frames = std::move(*frames);
				report.diagnostics.push_back(
					{validation_error.code, validation_error.field, validation_error.detail});
				return report;
			}

			detail::provider_process_validation_outcome report;
			report.terminal = terminal->reason;
			report.validated_transcript_success =
				terminal->kind == detail::transcript_terminal_kind::complete;
			report.sealed = std::move(*terminal).take_sealed();
			report.sealing_error = std::move(*terminal).take_sealing_error();
			if (report.validated_transcript_success && report.sealed)
			{
				auto receipt = detail::make_provider_runtime_receipt(raw_stdout_byte_count,
																	 std::move(raw_stdout_sha256),
																	 *frames,
																	 request.task_id,
																	 report.terminal,
																	 *report.sealed);
				if (!receipt)
					return cxxlens::sdk::unexpected(std::move(receipt.error()));
				report.runtime_receipt = std::move(*receipt);
			}
			report.input_seal = std::move(input_seal);
			report.provider_identity = std::move(prepared.provider_identity);
			report.provider = selected_manifest;
			report.task_input_digest = request.task_input_digest;
			report.normalized_invocation_digest = request.normalized_invocation_digest;
			report.toolchain_digest = request.toolchain_digest;
			report.environment_digest = request.environment_digest;
			report.measured_executable_digest = std::move(output.measured_executable_digest);
			report.sandbox = std::move(output.sandbox);
			report.frames = std::move(*frames);
			report.exit_code = output.exit_code;
			report.termination_signal = output.termination_signal;
			if (!output.standard_error.empty())
				report.diagnostics.push_back(
					{"provider.worker-stderr", request.task_id, std::move(output.standard_error)});
			return report;
		}
	} // namespace

	result<detail::provider_process_validation_outcome>
	detail::execute_provider_process(const provider_process_port& processes,
									 const process_task_request& request)
	{
		auto prepared = prepare_provider_process(request);
		if (!prepared)
			return cxxlens::sdk::unexpected(std::move(prepared.error()));
		vector_host_input input{request.payload};
		vector_frame_output transcript;
		auto input_seal = encode_host_transcript_incremental(
			prepared->input_profile, request.output_credit, input, transcript);
		if (!input_seal)
			return cxxlens::sdk::unexpected(std::move(input_seal.error()));
		prepared->invocation.standard_input = std::move(transcript.bytes_);
		auto launched = processes.run(prepared->invocation, request.cancellation);
		if (!launched)
			return cxxlens::sdk::unexpected(std::move(launched.error()));
		return finish_provider_process(
			request, std::move(*prepared), std::move(*input_seal), std::move(*launched));
	}

	result<detail::provider_process_validation_outcome>
	detail::execute_provider_process_replayable(const replayable_provider_process_port& processes,
												const process_task_request& request,
												const replayable_host_input& input)
	{
		auto prepared = prepare_provider_process(request);
		if (!prepared)
			return cxxlens::sdk::unexpected(std::move(prepared.error()));
		std::optional<sealed_host_input> input_seal;
		std::uint64_t writer_calls{};
		auto launched = processes.run_replayable(
			prepared->invocation,
			[&](frame_sink& sink) -> result<void>
			{
				if (++writer_calls != 1U)
					return cxxlens::sdk::unexpected(runtime_error(
						"provider.protocol-state-invalid", request.task_id, "input-writer-reused"));
				auto sealed = encode_host_transcript_incremental(
					prepared->input_profile, request.output_credit, input, sink);
				if (!sealed)
					return cxxlens::sdk::unexpected(std::move(sealed.error()));
				input_seal = std::move(*sealed);
				return {};
			},
			request.cancellation);
		if (!launched)
			return cxxlens::sdk::unexpected(std::move(launched.error()));
		if (writer_calls != 1U || !input_seal)
			return cxxlens::sdk::unexpected(runtime_error(
				"provider.protocol-state-invalid", request.task_id, "input-writer-not-consumed"));
		return finish_provider_process(
			request, std::move(*prepared), std::move(*input_seal), std::move(*launched));
	}

#if !defined(CXXLENS_PROVIDER_RUNTIME_INTERNAL_ONLY)
	result<process_execution_report>
	process_provider_runtime::execute(const process_task_request& request) const
	{
		if (processes_ == nullptr)
			return cxxlens::sdk::unexpected(
				runtime_error("provider.runtime-unavailable", "process-port"));
		auto outcome = detail::execute_provider_process(*processes_, request);
		if (!outcome)
			return cxxlens::sdk::unexpected(std::move(outcome.error()));

		process_execution_report report;
		report.terminal = std::move(outcome->terminal);
		report.provider = std::move(outcome->provider);
		report.task_input_digest = std::move(outcome->task_input_digest);
		report.normalized_invocation_digest = std::move(outcome->normalized_invocation_digest);
		report.toolchain_digest = std::move(outcome->toolchain_digest);
		report.environment_digest = std::move(outcome->environment_digest);
		report.measured_executable_digest = std::move(outcome->measured_executable_digest);
		report.sandbox = std::move(outcome->sandbox);
		report.frames = std::move(outcome->frames);
		report.diagnostics = std::move(outcome->diagnostics);
		report.exit_code = outcome->exit_code;
		report.termination_signal = outcome->termination_signal;
		report.validated_success_ = outcome->validated_transcript_success;
		return report;
	}
#endif
} // namespace cxxlens::sdk::provider
