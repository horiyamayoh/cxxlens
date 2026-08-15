#include "provider_ng1_spill_port_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
		constexpr std::size_t spill_length_prefix_bytes = sizeof(std::uint64_t);
		constexpr std::string_view spill_schema{"cxxlens.provider-spill-record.v1"};

		[[nodiscard]] error port_error(const std::string_view field, const std::string_view detail)
		{
			return {"provider.recovery-failed", std::string{field}, std::string{detail}};
		}

		[[nodiscard]] error corrupt_error(const std::string_view field,
										  const std::string_view detail)
		{
			return {"provider.spill-corrupt", std::string{field}, std::string{detail}};
		}

		[[nodiscard]] result<void> valid_semantic_digest(const std::string_view value,
														 const std::string_view field)
		{
			constexpr std::string_view prefix{"semantic-v2:sha256:"};
			if (!value.starts_with(prefix) || value.size() != prefix.size() + 64U)
				return unexpected(corrupt_error(field, "semantic-v2"));
			for (const auto byte : value.substr(prefix.size()))
				if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
					return unexpected(corrupt_error(field, "semantic-v2"));
			return {};
		}

		enum class cbor_major : std::uint8_t
		{
			unsigned_integer = 0U,
			bytes = 2U,
			text = 3U,
			map = 5U,
		};

		void append_big_endian(std::vector<std::byte>& output,
							   const std::uint64_t value,
							   const std::size_t width)
		{
			for (std::size_t index = width; index > 0U; --index)
				output.push_back(static_cast<std::byte>(value >> ((index - 1U) * 8U)));
		}

		void append_cbor_head(std::vector<std::byte>& output,
							  const cbor_major major,
							  const std::uint64_t value)
		{
			const auto prefix = static_cast<std::uint8_t>(static_cast<std::uint8_t>(major) << 5U);
			if (value < 24U)
				output.push_back(static_cast<std::byte>(prefix | static_cast<std::uint8_t>(value)));
			else if (value <= std::numeric_limits<std::uint8_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 24U));
				append_big_endian(output, value, 1U);
			}
			else if (value <= std::numeric_limits<std::uint16_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 25U));
				append_big_endian(output, value, 2U);
			}
			else if (value <= std::numeric_limits<std::uint32_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 26U));
				append_big_endian(output, value, 4U);
			}
			else
			{
				output.push_back(static_cast<std::byte>(prefix | 27U));
				append_big_endian(output, value, 8U);
			}
		}

		[[nodiscard]] result<std::vector<std::byte>> cbor_text(const std::string_view value,
															   const std::string_view field)
		{
			if (auto valid = validate_utf8_text(value); !valid)
				return unexpected(corrupt_error(field, "invalid-utf8"));
			try
			{
				std::vector<std::byte> output;
				output.reserve(9U + value.size());
				append_cbor_head(output, cbor_major::text, value.size());
				for (const auto byte : value)
					output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error(field, "allocation"));
			}
		}

		[[nodiscard]] result<std::vector<std::byte>>
		cbor_bytes(const std::span<const std::byte> value, const std::string_view field)
		{
			try
			{
				std::vector<std::byte> output;
				output.reserve(9U + value.size());
				append_cbor_head(output, cbor_major::bytes, value.size());
				output.insert(output.end(), value.begin(), value.end());
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error(field, "allocation"));
			}
		}

		struct encoded_field
		{
			std::vector<std::byte> key;
			std::vector<std::byte> value;
		};

		[[nodiscard]] result<std::vector<std::byte>>
		encode_spill_record(const ng1_spill_record& record)
		{
			// The prefix stores the deterministic-CBOR body length.  The complete wire
			// record, including this eight-byte framing prefix, is the quota/accounting
			// unit shared with ng1_spill_prefix_state::spill_record_wire_bytes().
			if (record.schema != spill_schema)
				return unexpected(corrupt_error("schema", "unexpected"));
			if (auto digest = ng1_spill_record_digest(record); !digest)
				return unexpected(std::move(digest.error()));

			try
			{
				std::vector<encoded_field> fields;
				fields.reserve(11U);
				auto add_text = [&fields](const std::string_view key,
										  const std::string_view value) -> result<void>
				{
					auto encoded_key = cbor_text(key, "record-key");
					if (!encoded_key)
						return unexpected(std::move(encoded_key.error()));
					auto encoded_value = cbor_text(value, key);
					if (!encoded_value)
						return unexpected(std::move(encoded_value.error()));
					fields.push_back({std::move(*encoded_key), std::move(*encoded_value)});
					return {};
				};
				auto add_uint = [&fields](const std::string_view key,
										  const std::uint64_t value) -> result<void>
				{
					auto encoded_key = cbor_text(key, "record-key");
					if (!encoded_key)
						return unexpected(std::move(encoded_key.error()));
					std::vector<std::byte> encoded_value;
					encoded_value.reserve(9U);
					append_cbor_head(encoded_value, cbor_major::unsigned_integer, value);
					fields.push_back({std::move(*encoded_key), std::move(encoded_value)});
					return {};
				};
				auto add_bytes = [&fields](const std::string_view key,
										   const std::span<const std::byte> value) -> result<void>
				{
					auto encoded_key = cbor_text(key, "record-key");
					if (!encoded_key)
						return unexpected(std::move(encoded_key.error()));
					auto encoded_value = cbor_bytes(value, key);
					if (!encoded_value)
						return unexpected(std::move(encoded_value.error()));
					fields.push_back({std::move(*encoded_key), std::move(*encoded_value)});
					return {};
				};

				for (const auto& outcome :
					 {add_text("schema", record.schema),
					  add_uint("record_ordinal", record.record_ordinal),
					  add_text("task_id", record.task_id),
					  add_text("dependency_group_id", record.dependency_group_id),
					  add_text("atomic_output_group_id", record.atomic_output_group_id),
					  add_text("batch_id", record.batch_id),
					  add_uint("stream_id", record.stream_id),
					  add_uint("sequence", record.sequence),
					  add_bytes("payload_bytes", record.payload_bytes),
					  add_text("payload_digest", record.payload_digest),
					  add_text("record_digest", record.record_digest)})
					if (!outcome)
						return unexpected(std::move(outcome.error()));

				std::ranges::sort(fields,
								  [](const encoded_field& left, const encoded_field& right)
								  {
									  if (left.key.size() != right.key.size())
										  return left.key.size() < right.key.size();
									  return std::lexicographical_compare(left.key.begin(),
																		  left.key.end(),
																		  right.key.begin(),
																		  right.key.end());
								  });

				std::vector<std::byte> body;
				body.reserve(1U + fields.size() * 8U + record.payload_bytes.size());
				append_cbor_head(body, cbor_major::map, fields.size());
				for (const auto& field : fields)
				{
					body.insert(body.end(), field.key.begin(), field.key.end());
					body.insert(body.end(), field.value.begin(), field.value.end());
				}
				if (body.size() > ng1_spill_maximum_record_bytes - spill_length_prefix_bytes)
					return unexpected(corrupt_error("record_bytes", "record-quota"));

				std::vector<std::byte> output;
				output.reserve(spill_length_prefix_bytes + body.size());
				append_big_endian(
					output, static_cast<std::uint64_t>(body.size()), spill_length_prefix_bytes);
				output.insert(output.end(), body.begin(), body.end());
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error("record", "allocation"));
			}
		}

		[[nodiscard]] result<std::pair<std::uint64_t, std::size_t>>
		decode_argument(const std::span<const std::byte> input,
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
				return unexpected(corrupt_error("cbor", "truncated-or-indefinite"));
			std::uint64_t value{};
			for (std::size_t index{}; index < width; ++index)
				value = (value << 8U) | std::to_integer<std::uint64_t>(input[offset + index]);
			const auto minimum = width == 1U ? 24U : (std::uint64_t{1U} << (width * 8U - 8U));
			if (value < minimum)
				return unexpected(corrupt_error("cbor", "non-shortest"));
			return std::pair{value, offset + width};
		}

		using decoded_scalar = std::variant<std::uint64_t, std::string, std::span<const std::byte>>;
		using decoded_map = std::map<std::string, decoded_scalar, std::less<>>;

		template <typename T>
		[[nodiscard]] result<void> require_value(const result<T>& value)
		{
			if (!value)
				return unexpected(value.error());
			return {};
		}

		[[nodiscard]] result<std::pair<decoded_scalar, std::size_t>>
		decode_scalar(const std::span<const std::byte> input, const std::size_t offset)
		{
			if (offset >= input.size())
				return unexpected(corrupt_error("cbor", "truncated"));
			const auto initial = std::to_integer<std::uint8_t>(input[offset]);
			const auto major = static_cast<cbor_major>(initial >> 5U);
			auto argument = decode_argument(input, offset + 1U, initial & 0x1fU);
			if (!argument)
				return unexpected(std::move(argument.error()));
			if (major == cbor_major::unsigned_integer)
				return std::pair{decoded_scalar{argument->first}, argument->second};
			if (argument->first > input.size() - argument->second)
				return unexpected(corrupt_error("cbor", "value-length"));
			const auto begin = argument->second;
			const auto end = begin + static_cast<std::size_t>(argument->first);
			if (major == cbor_major::text)
			{
				std::string value{reinterpret_cast<const char*>(input.data() + begin),
								  static_cast<std::size_t>(argument->first)};
				if (auto valid = validate_utf8_text(value); !valid)
					return unexpected(corrupt_error("cbor", "invalid-utf8"));
				return std::pair{decoded_scalar{std::move(value)}, end};
			}
			if (major == cbor_major::bytes)
				return std::pair{
					decoded_scalar{input.subspan(begin, static_cast<std::size_t>(argument->first))},
					end};
			return unexpected(corrupt_error("cbor", "scalar-type"));
		}

		[[nodiscard]] result<decoded_map> decode_map(const std::span<const std::byte> input)
		{
			if (input.empty())
				return unexpected(corrupt_error("cbor", "empty"));
			const auto initial = std::to_integer<std::uint8_t>(input.front());
			if ((initial >> 5U) != static_cast<std::uint8_t>(cbor_major::map))
				return unexpected(corrupt_error("cbor", "map-type"));
			auto count = decode_argument(input, 1U, initial & 0x1fU);
			if (!count)
				return unexpected(std::move(count.error()));
			if (count->first != 11U)
				return unexpected(corrupt_error("cbor", "field-count"));

			try
			{
				decoded_map output;
				std::vector<std::byte> previous_key;
				std::size_t offset = count->second;
				for (std::uint64_t index{}; index < count->first; ++index)
				{
					const auto key_begin = offset;
					auto key = decode_scalar(input, offset);
					if (!key || !std::holds_alternative<std::string>(key->first))
						return unexpected(corrupt_error("cbor", "map-key-type"));
					offset = key->second;
					std::vector<std::byte> encoded_key(
						input.begin() + static_cast<std::ptrdiff_t>(key_begin),
						input.begin() + static_cast<std::ptrdiff_t>(offset));
					if (!previous_key.empty())
					{
						const auto ordered = previous_key.size() < encoded_key.size() ||
							(previous_key.size() == encoded_key.size() &&
							 std::lexicographical_compare(previous_key.begin(),
														  previous_key.end(),
														  encoded_key.begin(),
														  encoded_key.end()));
						if (!ordered)
							return unexpected(corrupt_error("cbor", "map-order-or-duplicate"));
					}
					previous_key = std::move(encoded_key);

					auto value = decode_scalar(input, offset);
					if (!value)
						return unexpected(std::move(value.error()));
					offset = value->second;
					const auto& key_text = std::get<std::string>(key->first);
					if (!output.emplace(key_text, std::move(value->first)).second)
						return unexpected(corrupt_error(key_text, "duplicate-key"));
				}
				if (offset != input.size())
					return unexpected(corrupt_error("cbor", "trailing"));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error("cbor", "allocation"));
			}
		}

		template <typename T>
		[[nodiscard]] result<T> map_field(const decoded_map& fields, const std::string_view name)
		{
			const auto found = fields.find(name);
			if (found == fields.end())
				return unexpected(corrupt_error(name, "missing-field"));
			const auto* value = std::get_if<T>(&found->second);
			if (value == nullptr)
				return unexpected(corrupt_error(name, "field-type"));
			return *value;
		}

		[[nodiscard]] result<ng1_spill_record>
		decode_spill_record(const std::span<const std::byte> body)
		{
			auto fields = decode_map(body);
			if (!fields)
				return unexpected(std::move(fields.error()));
			auto schema = map_field<std::string>(*fields, "schema");
			auto ordinal = map_field<std::uint64_t>(*fields, "record_ordinal");
			auto task = map_field<std::string>(*fields, "task_id");
			auto dependency = map_field<std::string>(*fields, "dependency_group_id");
			auto atomic = map_field<std::string>(*fields, "atomic_output_group_id");
			auto batch = map_field<std::string>(*fields, "batch_id");
			auto stream = map_field<std::uint64_t>(*fields, "stream_id");
			auto sequence = map_field<std::uint64_t>(*fields, "sequence");
			auto payload = map_field<std::span<const std::byte>>(*fields, "payload_bytes");
			auto payload_digest = map_field<std::string>(*fields, "payload_digest");
			auto record_digest = map_field<std::string>(*fields, "record_digest");
			for (const auto& value : {require_value(schema),
									  require_value(ordinal),
									  require_value(task),
									  require_value(dependency),
									  require_value(atomic),
									  require_value(batch),
									  require_value(stream),
									  require_value(sequence),
									  require_value(payload),
									  require_value(payload_digest),
									  require_value(record_digest)})
				if (!value)
					return unexpected(std::move(value.error()));
			if (*schema != spill_schema)
				return unexpected(corrupt_error("schema", "unexpected"));

			auto expected_payload_digest = ng1_spill_payload_digest(*payload);
			if (!expected_payload_digest || *payload_digest != *expected_payload_digest)
				return unexpected(corrupt_error("payload_digest", "mismatch"));
			try
			{
				ng1_spill_record output{*schema,
										*ordinal,
										*task,
										*dependency,
										*atomic,
										*batch,
										*stream,
										*sequence,
										std::vector<std::byte>{payload->begin(), payload->end()},
										*payload_digest,
										*record_digest};
				auto expected_record_digest = ng1_spill_record_digest(output);
				if (!expected_record_digest || *record_digest != *expected_record_digest)
					return unexpected(corrupt_error("record_digest", "mismatch"));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error("record", "allocation"));
			}
		}

		[[nodiscard]] std::uint64_t read_big_endian(const std::span<const std::byte> input) noexcept
		{
			std::uint64_t value{};
			for (const auto byte : input)
				value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
			return value;
		}

		[[nodiscard]] result<ng1_spill_record>
		decode_framed_record(const std::span<const std::byte> bytes, std::size_t& offset)
		{
			// Validate the bounded body length before decoding the CBOR map.  The
			// eight-byte prefix remains part of the total wire-byte quota.
			if (offset > bytes.size() || bytes.size() - offset < spill_length_prefix_bytes)
				return unexpected(corrupt_error("framing", "truncated-length-prefix"));
			const auto length = read_big_endian(bytes.subspan(offset, spill_length_prefix_bytes));
			if (length == 0U || length > ng1_spill_maximum_record_bytes - spill_length_prefix_bytes)
				return unexpected(corrupt_error("record_bytes", "record-quota"));
			offset += spill_length_prefix_bytes;
			if (length > bytes.size() - offset)
				return unexpected(corrupt_error("framing", "torn-last-record"));
			const auto body = bytes.subspan(offset, static_cast<std::size_t>(length));
			offset += static_cast<std::size_t>(length);
			return decode_spill_record(body);
		}

#if defined(__linux__) && defined(__GLIBC__) && defined(SYS_memfd_create)
		class linux_ng1_spill_storage_port final : public ng1_spill_storage_port
		{
		  public:
			explicit linux_ng1_spill_storage_port(const int descriptor) noexcept
				: descriptor_{descriptor}
			{
			}

			~linux_ng1_spill_storage_port() override
			{
				if (descriptor_ >= 0)
					(void)::close(descriptor_);
			}

			[[nodiscard]] result<void> append(const std::span<const std::byte> bytes) override
			{
				if (descriptor_ < 0 || poisoned_ || cleaned_)
					return unexpected(port_error("append", "terminal-port"));
				if (bytes.size() > ng1_spill_maximum_total_bytes - byte_count_)
					return unexpected(corrupt_error("total_bytes", "total-quota"));
				const auto maximum_offset =
					static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
				if (byte_count_ > maximum_offset || bytes.size() > maximum_offset - byte_count_)
					return unexpected(port_error("append", "offset-overflow"));
				std::size_t consumed{};
				while (consumed < bytes.size())
				{
					const auto count = ::pwrite(descriptor_,
												bytes.data() + consumed,
												bytes.size() - consumed,
												static_cast<off_t>(byte_count_ + consumed));
					if (count > 0)
					{
						consumed += static_cast<std::size_t>(count);
						continue;
					}
					if (count < 0 && errno == EINTR)
						continue;
					poisoned_ = true;
					return unexpected(port_error("append", count == 0 ? "zero-write" : "write"));
				}
				byte_count_ += static_cast<std::uint64_t>(bytes.size());
				return {};
			}

			[[nodiscard]] result<std::uint64_t> fsync() override
			{
				if (descriptor_ < 0 || cleaned_ || poisoned_)
					return unexpected(port_error("fsync", "terminal-port"));
				if (::fsync(descriptor_) != 0)
				{
					poisoned_ = true;
					return unexpected(port_error("fsync", "durability-unknown"));
				}
				if (fsync_sequence_ == std::numeric_limits<std::uint64_t>::max())
				{
					poisoned_ = true;
					return unexpected(port_error("fsync_sequence", "overflow"));
				}
				++fsync_sequence_;
				return fsync_sequence_;
			}

			[[nodiscard]] result<std::vector<std::byte>> read_all() const override
			{
				if (descriptor_ < 0 || cleaned_)
					return unexpected(port_error("read", "terminal-port"));
				struct stat metadata{};
				if (::fstat(descriptor_, &metadata) != 0 || metadata.st_size < 0)
					return unexpected(port_error("read", "stat"));
				const auto size = static_cast<std::uint64_t>(metadata.st_size);
				if (size > ng1_spill_maximum_total_bytes || size != byte_count_)
					return unexpected(corrupt_error("total_bytes", "storage-drift"));
				try
				{
					std::vector<std::byte> output(static_cast<std::size_t>(size));
					std::size_t consumed{};
					while (consumed < output.size())
					{
						const auto count = ::pread(descriptor_,
												   output.data() + consumed,
												   output.size() - consumed,
												   static_cast<off_t>(consumed));
						if (count > 0)
						{
							consumed += static_cast<std::size_t>(count);
							continue;
						}
						if (count < 0 && errno == EINTR)
							continue;
						return unexpected(corrupt_error("framing", "torn-last-record"));
					}
					return output;
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(port_error("read", "allocation"));
				}
			}

			[[nodiscard]] result<void> cleanup() override
			{
				if (cleaned_)
					return unexpected(port_error("cleanup", "already-terminal"));
				cleaned_ = true;
				const auto descriptor = std::exchange(descriptor_, -1);
				if (descriptor < 0)
					return unexpected(port_error("cleanup", "missing-descriptor"));
				if (::close(descriptor) != 0)
					return unexpected(port_error("cleanup", "effect-unknown"));
				return {};
			}

		  private:
			int descriptor_{-1};
			std::uint64_t byte_count_{};
			std::uint64_t fsync_sequence_{};
			bool poisoned_{};
			bool cleaned_{};
		};
#endif
	} // namespace

	result<std::unique_ptr<ng1_spill_storage_port>> make_system_ng1_spill_storage_port()
	{
#if defined(__linux__) && defined(__GLIBC__) && defined(SYS_memfd_create)
		const auto descriptor = static_cast<int>(
			::syscall(SYS_memfd_create, "cxxlens-ng1-spill-v1", MFD_CLOEXEC | MFD_ALLOW_SEALING));
		if (descriptor < 0)
			return unexpected(port_error("platform", "memfd-create"));
		try
		{
			return std::unique_ptr<ng1_spill_storage_port>{
				std::make_unique<linux_ng1_spill_storage_port>(descriptor)};
		}
		catch (const std::bad_alloc&)
		{
			(void)::close(descriptor);
			return unexpected(port_error("platform", "allocation"));
		}
#else
		return unexpected(port_error("platform", "linux-glibc-memfd-required"));
#endif
	}

	ng1_spill_staging_session::ng1_spill_staging_session(
		ng1_spill_prefix_state prefix,
		ng1_spill_binding binding,
		std::unique_ptr<ng1_spill_storage_port> storage) noexcept
		: prefix_{std::move(prefix)}, binding_{std::move(binding)}, storage_{std::move(storage)}
	{
	}

	ng1_spill_staging_session::~ng1_spill_staging_session() noexcept
	{
		if (storage_ && !cleaned_)
			std::terminate();
	}

	ng1_spill_staging_session::ng1_spill_staging_session(ng1_spill_staging_session&& other) noexcept
		: prefix_{std::move(other.prefix_)}, binding_{std::move(other.binding_)},
		  storage_{std::move(other.storage_)}, last_fsync_sequence_{other.last_fsync_sequence_},
		  has_fsync_sequence_{other.has_fsync_sequence_}, poisoned_{other.poisoned_},
		  cleaned_{other.cleaned_}
	{
		other.cleaned_ = true;
		other.poisoned_ = true;
	}

	ng1_spill_staging_session&
	ng1_spill_staging_session::operator=(ng1_spill_staging_session&& other) noexcept
	{
		if (this == &other)
			return *this;
		if (storage_ && !cleaned_)
			std::terminate();
		prefix_ = std::move(other.prefix_);
		binding_ = std::move(other.binding_);
		storage_ = std::move(other.storage_);
		last_fsync_sequence_ = other.last_fsync_sequence_;
		has_fsync_sequence_ = other.has_fsync_sequence_;
		poisoned_ = other.poisoned_;
		cleaned_ = other.cleaned_;
		other.cleaned_ = true;
		other.poisoned_ = true;
		return *this;
	}

	result<ng1_spill_staging_session>
	ng1_spill_staging_session::create(ng1_spill_binding binding,
									  std::unique_ptr<ng1_spill_storage_port> storage)
	{
		if (!storage)
			return unexpected(port_error("storage", "missing-port"));
		auto prefix = ng1_spill_prefix_state::create(binding);
		if (!prefix)
			return unexpected(std::move(prefix.error()));
		return ng1_spill_staging_session{
			std::move(*prefix), std::move(binding), std::move(storage)};
	}

	result<void> ng1_spill_staging_session::append(const ng1_spill_record& record)
	{
		if (!storage_ || cleaned_ || poisoned_)
			return unexpected(port_error("append", "terminal-session"));
		try
		{
			auto candidate = prefix_;
			if (auto valid = candidate.append(record); !valid)
				return unexpected(std::move(valid.error()));
			auto wire = encode_spill_record(record);
			if (!wire)
				return unexpected(std::move(wire.error()));
			result<void> stored;
			try
			{
				stored = storage_->append(*wire);
			}
			catch (...)
			{
				poisoned_ = true;
				return unexpected(port_error("append", "effect-unknown"));
			}
			if (!stored)
			{
				poisoned_ = true;
				return unexpected(std::move(stored.error()));
			}
			prefix_ = std::move(candidate);
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(port_error("append", "allocation"));
		}
	}

	result<ng1_spill_fsync_receipt>
	ng1_spill_staging_session::fsync(const std::uint64_t highest_contiguous_acked_sequence,
									 const std::uint64_t highest_observed_sequence,
									 std::string staged_digest)
	{
		if (!storage_ || cleaned_ || poisoned_)
			return unexpected(port_error("fsync", "terminal-session"));
		if (highest_contiguous_acked_sequence > highest_observed_sequence)
			return unexpected(
				corrupt_error("highest_contiguous_acked_sequence", "ahead-of-observed"));
		if (auto valid = prefix_.validate_ack_frontier(highest_contiguous_acked_sequence); !valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = valid_semantic_digest(staged_digest, "staged_digest"); !valid)
			return unexpected(std::move(valid.error()));
		result<std::uint64_t> sequence{port_error("fsync", "not-called")};
		try
		{
			sequence = storage_->fsync();
		}
		catch (...)
		{
			poisoned_ = true;
			return unexpected(port_error("fsync", "effect-unknown"));
		}
		if (!sequence)
		{
			poisoned_ = true;
			return unexpected(std::move(sequence.error()));
		}
		if (*sequence == 0U || (has_fsync_sequence_ && *sequence <= last_fsync_sequence_))
		{
			poisoned_ = true;
			return unexpected(port_error("fsync_sequence", "not-increasing"));
		}
		result<ng1_spill_fsync_receipt> receipt{port_error("fsync", "not-observed")};
		try
		{
			receipt = prefix_.observe_host_fsync(
				highest_contiguous_acked_sequence, std::move(staged_digest), *sequence);
		}
		catch (...)
		{
			poisoned_ = true;
			return unexpected(port_error("fsync", "effect-unknown"));
		}
		if (!receipt)
		{
			poisoned_ = true;
			return unexpected(std::move(receipt.error()));
		}
		last_fsync_sequence_ = *sequence;
		has_fsync_sequence_ = true;
		return receipt;
	}

	result<ng1_spill_prefix_state> ng1_spill_staging_session::recover()
	{
		if (!storage_ || cleaned_)
			return unexpected(port_error("recovery", "terminal-session"));
		result<std::vector<std::byte>> raw{port_error("recovery", "not-read")};
		try
		{
			raw = storage_->read_all();
		}
		catch (...)
		{
			poisoned_ = true;
			return unexpected(port_error("recovery", "effect-unknown"));
		}
		if (!raw)
		{
			poisoned_ = true;
			return unexpected(std::move(raw.error()));
		}
		try
		{
			auto recovered = ng1_spill_prefix_state::create(binding_);
			if (!recovered)
			{
				poisoned_ = true;
				return unexpected(std::move(recovered.error()));
			}
			std::size_t offset{};
			while (offset < raw->size())
			{
				auto record = decode_framed_record(*raw, offset);
				if (!record)
				{
					poisoned_ = true;
					return unexpected(std::move(record.error()));
				}
				if (auto admitted = recovered->append(*record); !admitted)
				{
					poisoned_ = true;
					return unexpected(std::move(admitted.error()));
				}
			}
			if (recovered->total_bytes() != raw->size())
			{
				poisoned_ = true;
				return unexpected(corrupt_error("total_bytes", "framing-mismatch"));
			}
			return recovered;
		}
		catch (...)
		{
			poisoned_ = true;
			return unexpected(port_error("recovery", "effect-unknown"));
		}
	}

	result<void> ng1_spill_staging_session::cleanup()
	{
		if (!storage_ || cleaned_)
			return unexpected(port_error("cleanup", "already-terminal"));
		cleaned_ = true;
		result<void> result;
		try
		{
			result = storage_->cleanup();
		}
		catch (...)
		{
			poisoned_ = true;
			storage_.reset();
			return unexpected(port_error("cleanup", "effect-unknown"));
		}
		storage_.reset();
		if (!result)
		{
			poisoned_ = true;
			return unexpected(std::move(result.error()));
		}
		return {};
	}
} // namespace cxxlens::sdk::provider::detail
