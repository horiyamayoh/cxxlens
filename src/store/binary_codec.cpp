#include "binary_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cxxlens::detail::store
{
	namespace
	{
		constexpr std::array<std::byte, 8> magic{std::byte{'C'},
												 std::byte{'X'},
												 std::byte{'X'},
												 std::byte{'L'},
												 std::byte{'E'},
												 std::byte{'N'},
												 std::byte{'S'},
												 std::byte{1}};
		constexpr std::uint32_t format_version{1};
		constexpr std::uint64_t maximum_collection_size{1'000'000};

		[[nodiscard]] error codec_error(std::string reason)
		{
			error failure;
			failure.code.value = "facts.store-corrupt";
			failure.message = "Fact snapshot binary is invalid";
			failure.scope = failure_scope::workspace;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		class writer
		{
		  public:
			template <typename Integer>
			void integer(Integer value)
			{
				using unsigned_type = std::make_unsigned_t<Integer>;
				auto encoded = static_cast<unsigned_type>(value);
				for (std::size_t index{}; index < sizeof(Integer); ++index)
				{
					bytes_.push_back(static_cast<std::byte>(encoded & 0xffU));
					encoded = static_cast<unsigned_type>(encoded / 256U);
				}
			}

			void boolean(const bool value)
			{
				integer<std::uint8_t>(value ? 1U : 0U);
			}

			void string(const std::string_view value)
			{
				integer<std::uint64_t>(value.size());
				for (const char character : value)
					bytes_.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
			}

			void raw(const std::span<const std::byte> value)
			{
				bytes_.insert(bytes_.end(), value.begin(), value.end());
			}

			[[nodiscard]] std::vector<std::byte> finish() &&
			{
				return std::move(bytes_);
			}

		  private:
			std::vector<std::byte> bytes_;
		};

		class reader
		{
		  public:
			explicit reader(const std::span<const std::byte> bytes) : bytes_{bytes} {}

			template <typename Integer>
			[[nodiscard]] std::optional<Integer> integer()
			{
				if (remaining() < sizeof(Integer))
					return std::nullopt;
				using unsigned_type = std::make_unsigned_t<Integer>;
				unsigned_type decoded{};
				for (std::size_t index{}; index < sizeof(Integer); ++index)
					decoded |= static_cast<unsigned_type>(
								   std::to_integer<unsigned char>(bytes_[offset_++]))
						<< (index * 8U);
				return static_cast<Integer>(decoded);
			}

			[[nodiscard]] std::optional<bool> boolean()
			{
				const auto value = integer<std::uint8_t>();
				if (!value || *value > 1U)
					return std::nullopt;
				return *value == 1U;
			}

			[[nodiscard]] std::optional<std::string> string()
			{
				const auto size = integer<std::uint64_t>();
				if (!size || *size > remaining() ||
					*size > std::numeric_limits<std::uint32_t>::max())
					return std::nullopt;
				std::string value;
				value.reserve(static_cast<std::size_t>(*size));
				for (std::uint64_t index{}; index < *size; ++index)
					value.push_back(
						static_cast<char>(std::to_integer<unsigned char>(bytes_[offset_++])));
				return value;
			}

			[[nodiscard]] bool consume(const std::span<const std::byte> expected)
			{
				if (remaining() < expected.size() ||
					!std::ranges::equal(expected, bytes_.subspan(offset_, expected.size())))
					return false;
				offset_ += expected.size();
				return true;
			}

			[[nodiscard]] std::size_t remaining() const noexcept
			{
				return bytes_.size() - offset_;
			}

		  private:
			std::span<const std::byte> bytes_;
			std::size_t offset_{};
		};

		[[nodiscard]] bool valid_collection_count(const reader& input,
												  const std::optional<std::uint64_t> count)
		{
			const auto value = count.value_or(maximum_collection_size + 1U);
			return value <= maximum_collection_size && value <= input.remaining();
		}

		void write_point(writer& output, const source_point& value)
		{
			output.string(value.file.value());
			output.integer(value.byte_offset);
			output.integer(value.line);
			output.integer(value.column);
			output.integer(static_cast<std::uint8_t>(value.state));
		}

		[[nodiscard]] bool read_point(reader& input, source_point& value)
		{
			auto file = input.string();
			auto offset = input.integer<std::uint64_t>();
			auto line = input.integer<std::uint32_t>();
			auto column = input.integer<std::uint32_t>();
			auto state = input.integer<std::uint8_t>();
			if (!file || !offset || !line || !column || !state ||
				*state > static_cast<std::uint8_t>(source_location_state::unknown))
				return false;
			value.file = file_id{std::move(*file)};
			value.byte_offset = *offset;
			value.line = *line;
			value.column = *column;
			value.state = static_cast<source_location_state>(*state);
			return true;
		}

		void write_range(writer& output, const file_range& value)
		{
			write_point(output, value.begin);
			write_point(output, value.end);
			output.integer(static_cast<std::uint8_t>(value.kind));
		}

		[[nodiscard]] bool read_range(reader& input, file_range& value)
		{
			if (!read_point(input, value.begin) || !read_point(input, value.end))
				return false;
			const auto kind = input.integer<std::uint8_t>();
			const auto decoded = kind.value_or(std::numeric_limits<std::uint8_t>::max());
			if (decoded > static_cast<std::uint8_t>(source_range_kind::token))
				return false;
			value.kind = static_cast<source_range_kind>(decoded);
			return true;
		}

		template <typename Value, typename Write>
		void write_optional(writer& output, const std::optional<Value>& value, Write write)
		{
			output.boolean(value.has_value());
			if (value)
				write(output, *value);
		}

		template <typename Value, typename Read>
		[[nodiscard]] bool read_optional(reader& input, std::optional<Value>& value, Read read)
		{
			const auto present = input.boolean();
			if (!present)
				return false;
			if (!*present)
			{
				value.reset();
				return true;
			}
			Value decoded;
			if (!read(input, decoded))
				return false;
			value = std::move(decoded);
			return true;
		}

		void write_span(writer& output, const source_span& value)
		{
			write_range(output, value.primary);
			write_optional(output,
						   value.spelling,
						   [](writer& nested, const file_range& range)
						   {
							   write_range(nested, range);
						   });
			write_optional(output,
						   value.expansion,
						   [](writer& nested, const file_range& range)
						   {
							   write_range(nested, range);
						   });
			output.integer<std::uint64_t>(value.macro_stack.size());
			for (const auto& frame : value.macro_stack)
			{
				output.string(frame.macro_name);
				write_range(output, frame.invocation);
				write_optional(output,
							   frame.definition,
							   [](writer& nested, const file_range& range)
							   {
								   write_range(nested, range);
							   });
				write_optional(output,
							   frame.argument_index,
							   [](writer& nested, const std::uint32_t index)
							   {
								   nested.integer(index);
							   });
			}
			output.integer(static_cast<std::uint8_t>(value.origin));
			output.string(value.digest.algorithm);
			output.integer(value.digest.version);
			output.string(value.digest.value);
			output.boolean(value.read_only);
		}

		[[nodiscard]] bool read_span(reader& input, source_span& value)
		{
			if (!read_range(input, value.primary) ||
				!read_optional(input,
							   value.spelling,
							   [](reader& nested, file_range& range)
							   {
								   return read_range(nested, range);
							   }) ||
				!read_optional(input,
							   value.expansion,
							   [](reader& nested, file_range& range)
							   {
								   return read_range(nested, range);
							   }))
				return false;
			const auto frame_count = input.integer<std::uint64_t>();
			if (!valid_collection_count(input, frame_count))
				return false;
			const auto decoded_frame_count = frame_count.value_or(0U);
			value.macro_stack.clear();
			value.macro_stack.reserve(static_cast<std::size_t>(decoded_frame_count));
			for (std::uint64_t index{}; index < decoded_frame_count; ++index)
			{
				macro_frame frame;
				auto name = input.string();
				if (!name || !read_range(input, frame.invocation) ||
					!read_optional(input,
								   frame.definition,
								   [](reader& nested, file_range& range)
								   {
									   return read_range(nested, range);
								   }) ||
					!read_optional(input,
								   frame.argument_index,
								   [](reader& nested, std::uint32_t& value)
								   {
									   const auto decoded = nested.integer<std::uint32_t>();
									   if (!decoded)
										   return false;
									   value = *decoded;
									   return true;
								   }))
					return false;
				frame.macro_name = std::move(*name);
				value.macro_stack.push_back(std::move(frame));
			}
			const auto origin = input.integer<std::uint8_t>();
			auto algorithm = input.string();
			const auto digest_version = input.integer<std::uint32_t>();
			auto digest = input.string();
			const auto read_only = input.boolean();
			if (!origin || *origin > static_cast<std::uint8_t>(source_origin::unknown) ||
				!algorithm || !digest_version || !digest || !read_only)
				return false;
			value.origin = static_cast<source_origin>(*origin);
			value.digest.algorithm = std::move(*algorithm);
			value.digest.version = *digest_version;
			value.digest.value = std::move(*digest);
			value.read_only = *read_only;
			return !value.validate();
		}

		void write_string_map(writer& output, const std::map<std::string, std::string>& values)
		{
			output.integer<std::uint64_t>(values.size());
			for (const auto& [key, value] : values)
			{
				output.string(key);
				output.string(value);
			}
		}

		[[nodiscard]] bool read_string_map(reader& input,
										   std::map<std::string, std::string>& values)
		{
			const auto count = input.integer<std::uint64_t>();
			if (!valid_collection_count(input, count))
				return false;
			const auto decoded_count = count.value_or(0U);
			values.clear();
			for (std::uint64_t index{}; index < decoded_count; ++index)
			{
				auto key = input.string();
				auto value = input.string();
				if (!key || !value || !values.emplace(std::move(*key), std::move(*value)).second)
					return false;
			}
			return true;
		}

		template <typename Id>
		void write_id_vector(writer& output, const std::vector<Id>& values)
		{
			output.integer<std::uint64_t>(values.size());
			for (const auto& value : values)
				output.string(value.value());
		}

		template <typename Id>
		[[nodiscard]] bool read_id_vector(reader& input, std::vector<Id>& values)
		{
			const auto count = input.integer<std::uint64_t>();
			if (!valid_collection_count(input, count))
				return false;
			const auto decoded_count = count.value_or(0U);
			values.clear();
			values.reserve(static_cast<std::size_t>(decoded_count));
			for (std::uint64_t index{}; index < decoded_count; ++index)
			{
				auto encoded = input.string();
				if (!encoded)
					return false;
				Id value{std::move(*encoded)};
				if (!value.valid())
					return false;
				values.push_back(std::move(value));
			}
			return true;
		}

		void write_fact(writer& output, const facts::detached_fact_record& value)
		{
			output.string(value.schema);
			output.string(value.id.value());
			output.integer(static_cast<std::uint16_t>(value.kind));
			output.string(value.stable_key);
			write_optional(output,
						   value.source,
						   [](writer& nested, const source_span& span)
						   {
							   write_span(nested, span);
						   });
			write_id_vector(output, value.origin.compile_units);
			write_id_vector(output, value.origin.variants);
			output.string(value.origin.extractor_id);
			output.string(value.origin.extractor_version);
			output.integer(value.payload_version);
			write_string_map(output, value.payload);
			write_optional(output,
						   value.name,
						   [](writer& nested, const facts::name_identity& name)
						   {
							   nested.string(name.display_qualified_name);
							   write_optional(nested,
											  name.usr,
											  [](writer& target, const std::string& text)
											  {
												  target.string(text);
											  });
							   write_optional(nested,
											  name.semantic_owner,
											  [](writer& target, const std::string& text)
											  {
												  target.string(text);
											  });
							   write_optional(nested,
											  name.declaration_kind,
											  [](writer& target, const std::string& text)
											  {
												  target.string(text);
											  });
							   write_optional(nested,
											  name.signature_structure,
											  [](writer& target, const std::string& text)
											  {
												  target.string(text);
											  });
						   });
			write_optional(output,
						   value.type,
						   [](writer& nested, const facts::type_identity& type)
						   {
							   nested.string(type.display_spelling);
							   nested.string(type.canonical_structure);
							   write_optional(nested,
											  type.declaration,
											  [](writer& target, const symbol_id& id)
											  {
												  target.string(id.value());
											  });
							   write_id_vector(nested, type.components);
							   nested.boolean(type.builtin);
						   });
		}

		[[nodiscard]] bool read_fact(reader& input, facts::detached_fact_record& value)
		{
			auto schema = input.string();
			auto id = input.string();
			const auto kind = input.integer<std::uint16_t>();
			auto stable_key = input.string();
			if (!schema || !id || !kind || *kind > static_cast<std::uint16_t>(fact_kind::custom) ||
				!stable_key ||
				!read_optional(input,
							   value.source,
							   [](reader& nested, source_span& span)
							   {
								   return read_span(nested, span);
							   }) ||
				!read_id_vector(input, value.origin.compile_units) ||
				!read_id_vector(input, value.origin.variants))
				return false;
			value.schema = std::move(*schema);
			value.id = fact_id{std::move(*id)};
			value.kind = static_cast<fact_kind>(*kind);
			value.stable_key = std::move(*stable_key);
			auto extractor_id = input.string();
			auto extractor_version = input.string();
			const auto payload_version = input.integer<std::uint32_t>();
			if (!extractor_id || !extractor_version || !payload_version ||
				!read_string_map(input, value.payload))
				return false;
			value.origin.extractor_id = std::move(*extractor_id);
			value.origin.extractor_version = std::move(*extractor_version);
			value.payload_version = *payload_version;
			if (!read_optional(input,
							   value.name,
							   [](reader& nested, facts::name_identity& name)
							   {
								   auto display = nested.string();
								   if (!display ||
									   !read_optional(nested,
													  name.usr,
													  [](reader& target, std::string& text)
													  {
														  auto value = target.string();
														  if (!value)
															  return false;
														  text = std::move(*value);
														  return true;
													  }) ||
									   !read_optional(nested,
													  name.semantic_owner,
													  [](reader& target, std::string& text)
													  {
														  auto value = target.string();
														  if (!value)
															  return false;
														  text = std::move(*value);
														  return true;
													  }) ||
									   !read_optional(nested,
													  name.declaration_kind,
													  [](reader& target, std::string& text)
													  {
														  auto value = target.string();
														  if (!value)
															  return false;
														  text = std::move(*value);
														  return true;
													  }) ||
									   !read_optional(nested,
													  name.signature_structure,
													  [](reader& target, std::string& text)
													  {
														  auto value = target.string();
														  if (!value)
															  return false;
														  text = std::move(*value);
														  return true;
													  }))
									   return false;
								   name.display_qualified_name = std::move(*display);
								   return true;
							   }) ||
				!read_optional(input,
							   value.type,
							   [](reader& nested, facts::type_identity& type)
							   {
								   auto spelling = nested.string();
								   auto canonical = nested.string();
								   if (!spelling || !canonical ||
									   !read_optional(nested,
													  type.declaration,
													  [](reader& target, symbol_id& id)
													  {
														  auto encoded = target.string();
														  if (!encoded)
															  return false;
														  id = symbol_id{std::move(*encoded)};
														  return id.valid();
													  }) ||
									   !read_id_vector(nested, type.components))
									   return false;
								   const auto builtin = nested.boolean();
								   if (!builtin)
									   return false;
								   type.display_spelling = std::move(*spelling);
								   type.canonical_structure = std::move(*canonical);
								   type.builtin = *builtin;
								   return true;
							   }))
				return false; // NOLINT(readability-simplify-boolean-expr)
			return true;
		}

		void write_coverage(writer& output, const coverage_report& coverage)
		{
			output.integer<std::uint64_t>(coverage.requested().size());
			for (const auto& request : coverage.requested())
			{
				output.string(request.kind);
				output.string(request.id);
			}
			output.integer<std::uint64_t>(coverage.units().size());
			for (const auto& unit : coverage.units())
			{
				output.string(unit.kind);
				output.string(unit.id);
				output.integer(static_cast<std::uint8_t>(unit.state));
				write_optional(output,
							   unit.reason,
							   [](writer& target, const std::string& reason)
							   {
								   target.string(reason);
							   });
			}
		}

		[[nodiscard]] bool read_coverage(reader& input, coverage_report& coverage)
		{
			const auto request_count = input.integer<std::uint64_t>();
			if (!valid_collection_count(input, request_count))
				return false;
			const auto decoded_request_count = request_count.value_or(0U);
			coverage = {};
			for (std::uint64_t index{}; index < decoded_request_count; ++index)
			{
				auto kind = input.string();
				auto id = input.string();
				if (!kind || !id)
					return false;
				coverage.request({std::move(*kind), std::move(*id)});
			}
			const auto unit_count = input.integer<std::uint64_t>();
			const auto decoded_unit_count = unit_count.value_or(maximum_collection_size + 1U);
			if (!valid_collection_count(input, unit_count))
				return false;
			for (std::uint64_t index{}; index < decoded_unit_count; ++index)
			{
				auto kind = input.string();
				auto id = input.string();
				const auto state = input.integer<std::uint8_t>();
				std::optional<std::string> reason;
				if (!kind || !id || !state ||
					*state > static_cast<std::uint8_t>(coverage_state::not_applicable) ||
					!read_optional(input,
								   reason,
								   [](reader& nested, std::string& text)
								   {
									   auto value = nested.string();
									   if (!value)
										   return false;
									   text = std::move(*value);
									   return true;
								   }))
					return false;
				coverage.classify({std::move(*kind),
								   std::move(*id),
								   static_cast<coverage_state>(*state),
								   std::move(reason)});
			}
			return true;
		}
	} // namespace

	result<std::vector<std::byte>> encode_snapshot(const snapshot_data& snapshot)
	{
		if (auto checked = snapshot.validate(); !checked)
			return codec_error("snapshot-validation-failed");
		writer output;
		output.raw(magic);
		output.integer(format_version);
		output.string(snapshot.metadata.workspace_key);
		output.string(snapshot.metadata.schema_version);
		output.string(snapshot.metadata.semantics_version);
		output.string(snapshot.metadata.adapter_id);
		output.string(snapshot.metadata.adapter_version);
		write_string_map(output, snapshot.metadata.extractor_versions);
		output.integer(snapshot.metadata.generation);
		output.integer<std::uint64_t>(snapshot.facts.size());
		for (const auto& fact : snapshot.facts)
			write_fact(output, fact);
		write_coverage(output, snapshot.coverage);
		return std::move(output).finish();
	}

	result<std::shared_ptr<snapshot_data>> decode_snapshot(const std::span<const std::byte> bytes)
	{
		reader input{bytes};
		const auto version = [&input]() -> std::optional<std::uint32_t>
		{
			if (!input.consume(magic))
				return std::nullopt;
			return input.integer<std::uint32_t>();
		}();
		if (!version || *version != format_version)
			return codec_error("binary-header-invalid");
		auto snapshot = std::make_shared<snapshot_data>();
		auto workspace = input.string();
		auto schema = input.string();
		auto semantics = input.string();
		auto adapter = input.string();
		auto adapter_version = input.string();
		if (!workspace || !schema || !semantics || !adapter || !adapter_version ||
			!read_string_map(input, snapshot->metadata.extractor_versions))
			return codec_error("metadata-truncated");
		snapshot->metadata.workspace_key = std::move(*workspace);
		snapshot->metadata.schema_version = std::move(*schema);
		snapshot->metadata.semantics_version = std::move(*semantics);
		snapshot->metadata.adapter_id = std::move(*adapter);
		snapshot->metadata.adapter_version = std::move(*adapter_version);
		const auto generation = input.integer<std::uint64_t>();
		const auto fact_count = input.integer<std::uint64_t>();
		if (!generation || !valid_collection_count(input, fact_count))
			return codec_error("fact-table-truncated");
		snapshot->metadata.generation = *generation;
		const auto decoded_fact_count = fact_count.value_or(0U);
		snapshot->facts.reserve(static_cast<std::size_t>(decoded_fact_count));
		for (std::uint64_t index{}; index < decoded_fact_count; ++index)
		{
			facts::detached_fact_record fact;
			if (!read_fact(input, fact))
				return codec_error("fact-row-invalid");
			snapshot->facts.push_back(std::move(fact));
		}
		if (!read_coverage(input, snapshot->coverage) || input.remaining() != 0U)
			return codec_error("binary-trailing-or-truncated");
		if (auto checked = snapshot->validate(); !checked)
			return codec_error("decoded-snapshot-invalid");
		return snapshot;
	}
} // namespace cxxlens::detail::store
