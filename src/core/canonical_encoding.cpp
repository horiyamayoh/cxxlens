#include "canonical_encoding.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <iomanip>
#include <ranges>
#include <set>
#include <sstream>

namespace cxxlens::detail::identity
{
	namespace
	{
		void append_varint(std::vector<std::byte>& output, std::uint64_t value)
		{
			do
			{
				auto byte = static_cast<std::uint8_t>(value & 0x7FU);
				value >>= 7U;
				if (value != 0U)
					byte |= 0x80U;
				output.push_back(static_cast<std::byte>(byte));
			} while (value != 0U);
		}

		void append_text(std::vector<std::byte>& output, const std::string_view value)
		{
			append_varint(output, value.size());
			const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
			output.insert(output.end(), bytes.begin(), bytes.end());
		}

		std::vector<std::byte> text_value(const std::string_view value)
		{
			std::vector<std::byte> output;
			append_text(output, value);
			return output;
		}

		bool valid_name(const std::string_view name)
		{
			return !name.empty() &&
				std::ranges::all_of(name,
									[](const char value)
									{
										return (value >= 'a' && value <= 'z') ||
											(value >= '0' && value <= '9') || value == '_';
									});
		}

		bool forbidden_name(const std::string_view name)
		{
			constexpr std::array names{"timestamp",
									   "wall_time",
									   "pid",
									   "thread_id",
									   "pointer",
									   "cache_hit",
									   "observation_order",
									   "message",
									   "diagnostic"};
			return std::ranges::find(names, name) != names.end();
		}

		bool valid_prefix(const std::string_view prefix)
		{
			return !prefix.empty() &&
				std::ranges::all_of(prefix,
									[](const char value)
									{
										return (value >= 'a' && value <= 'z') ||
											(value >= '0' && value <= '9') || value == '-';
									});
		}

		bool valid_project_key(const std::string_view key)
		{
			if (key.empty() || key.front() == '/' || key.front() == '\\' ||
				key.find('\\') != key.npos || key.find("//") != key.npos ||
				key.find('"') != key.npos)
				return false;
			std::size_t start{};
			while (start <= key.size())
			{
				const auto end = key.find('/', start);
				const auto part = key.substr(start, end == key.npos ? key.npos : end - start);
				if (part.empty() || part == "." || part == "..")
					return false;
				if (end == key.npos)
					break;
				start = end + 1U;
			}
			return true;
		}

		bool forbidden_role(const identity_field_role role)
		{
			return role != identity_field_role::semantic &&
				role != identity_field_role::project_relative_source;
		}
	} // namespace

	canonical_encoder::canonical_encoder(std::string domain, const encoding_versions versions)
		: domain_{std::move(domain)}, schema_version_{versions.schema},
		  semantics_version_{versions.semantics}
	{
		if (!domain_.starts_with("cxxlens.") || domain_.find(".v") == std::string::npos)
			reject(identity_error_code::invalid_domain, "domain");
		if (schema_version_ == 0U || semantics_version_ == 0U)
			reject(identity_error_code::missing_version, "version");
	}

	void canonical_encoder::reject(const identity_error_code code, std::string field)
	{
		if (!error_)
			error_ = identity_error{code, std::move(field)};
	}

	void
	canonical_encoder::add(std::string name, const std::byte type, std::vector<std::byte> value)
	{
		if (!valid_name(name))
			return reject(identity_error_code::invalid_field_name, std::move(name));
		if (forbidden_name(name))
			return reject(identity_error_code::forbidden_identity_input, std::move(name));
		std::vector<std::byte> encoded{type};
		append_varint(encoded, value.size());
		encoded.insert(encoded.end(), value.begin(), value.end());
		if (!fields_.emplace(name, std::move(encoded)).second)
			reject(identity_error_code::duplicate_field, std::move(name));
	}

	canonical_encoder& canonical_encoder::string_field(std::string name,
													   const std::string& value,
													   const identity_field_role role)
	{
		const bool forbidden = forbidden_role(role) ||
			(role == identity_field_role::semantic &&
			 (value.starts_with('/') || value.starts_with("file:/")));
		if (forbidden)
			reject(identity_error_code::forbidden_identity_input, name);
		else if (role == identity_field_role::project_relative_source && !valid_project_key(value))
			reject(identity_error_code::invalid_project_relative_key, name);
		add(std::move(name), std::byte{0x01}, text_value(value));
		return *this;
	}

	canonical_encoder& canonical_encoder::unsigned_field(std::string name,
														 const std::uint64_t value)
	{
		std::vector<std::byte> bytes;
		for (int shift = 56; shift >= 0; shift -= 8)
			bytes.push_back(
				static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU));
		add(std::move(name), std::byte{0x02}, std::move(bytes));
		return *this;
	}

	canonical_encoder& canonical_encoder::signed_field(std::string name, const std::int64_t value)
	{
		const auto encoded = std::bit_cast<std::uint64_t>(value);
		std::vector<std::byte> bytes;
		for (int shift = 56; shift >= 0; shift -= 8)
			bytes.push_back(
				static_cast<std::byte>((encoded >> static_cast<unsigned>(shift)) & 0xFFU));
		add(std::move(name), std::byte{0x03}, std::move(bytes));
		return *this;
	}

	canonical_encoder& canonical_encoder::boolean_field(std::string name, const bool value)
	{
		add(std::move(name), std::byte{0x04}, {value ? std::byte{1} : std::byte{0}});
		return *this;
	}

	canonical_encoder& canonical_encoder::enum_field(std::string name, const std::int64_t value)
	{
		const auto encoded = std::bit_cast<std::uint64_t>(value);
		std::vector<std::byte> bytes;
		for (int shift = 56; shift >= 0; shift -= 8)
			bytes.push_back(
				static_cast<std::byte>((encoded >> static_cast<unsigned>(shift)) & 0xFFU));
		add(std::move(name), std::byte{0x0A}, std::move(bytes));
		return *this;
	}

	canonical_encoder& canonical_encoder::bytes_field(std::string name,
													  const std::span<const std::byte> value)
	{
		add(std::move(name), std::byte{0x05}, {value.begin(), value.end()});
		return *this;
	}

	canonical_encoder& canonical_encoder::optional_string(std::string name,
														  const std::optional<std::string>& value)
	{
		std::vector<std::byte> bytes{value ? std::byte{1} : std::byte{0}};
		if (value)
		{
			auto text = text_value(*value);
			bytes.insert(bytes.end(), text.begin(), text.end());
		}
		add(std::move(name), std::byte{0x06}, std::move(bytes));
		return *this;
	}

	canonical_encoder& canonical_encoder::string_sequence(std::string name,
														  const std::vector<std::string>& values)
	{
		std::vector<std::byte> bytes;
		append_varint(bytes, values.size());
		for (const auto& value : values)
			append_text(bytes, value);
		add(std::move(name), std::byte{0x07}, std::move(bytes));
		return *this;
	}

	canonical_encoder& canonical_encoder::string_set(std::string name,
													 std::vector<std::string> values)
	{
		std::ranges::sort(values);
		values.erase(std::unique(values.begin(), values.end()), values.end());
		std::vector<std::byte> bytes;
		append_varint(bytes, values.size());
		for (const auto& value : values)
			append_text(bytes, value);
		add(std::move(name), std::byte{0x08}, std::move(bytes));
		return *this;
	}

	canonical_encoder&
	canonical_encoder::string_map(std::string name,
								  std::vector<std::pair<std::string, std::string>> values)
	{
		std::ranges::sort(values, {}, &std::pair<std::string, std::string>::first);
		for (std::size_t index = 1U; index < values.size(); ++index)
			if (values[index - 1U].first == values[index].first)
				reject(identity_error_code::duplicate_map_key, name);
		std::vector<std::byte> bytes;
		append_varint(bytes, values.size());
		for (const auto& [key, value] : values)
		{
			append_text(bytes, key);
			append_text(bytes, value);
		}
		add(std::move(name), std::byte{0x09}, std::move(bytes));
		return *this;
	}

	identity_result<canonical_payload> canonical_encoder::finish() const
	{
		if (error_)
			return *error_;
		std::vector<std::byte> bytes;
		append_text(bytes, "cxxlens.canonical.v1");
		append_text(bytes, domain_);
		append_varint(bytes, schema_version_);
		append_varint(bytes, semantics_version_);
		append_varint(bytes, fields_.size());
		for (const auto& [name, value] : fields_)
		{
			append_text(bytes, name);
			bytes.insert(bytes.end(), value.begin(), value.end());
		}
		return canonical_payload{domain_, schema_version_, semantics_version_, std::move(bytes)};
	}

	identity_result<bool> collision_registry::check_and_register(std::string stable_id,
																 std::string payload_fingerprint)
	{
		const auto [found, inserted] =
			fingerprints_.emplace(std::move(stable_id), payload_fingerprint);
		if (!inserted && found->second != payload_fingerprint)
			return identity_error{identity_error_code::collision, found->first};
		return true;
	}

	identity_service::identity_service(const runtime::hash_port& hashes) : hashes_{hashes} {}

	identity_result<std::string>
	identity_service::fingerprint(const canonical_payload& payload) const
	{
		std::string full;
		for (std::uint32_t lane = 0U; lane < 4U; ++lane)
		{
			runtime::request_context context;
			context.operation = "identity.hash";
			context.call_index = lane;
			const auto result = hashes_.calculate(
				runtime::hash_request{"fnv1a64",
									  1U,
									  "cxxlens.identity.lane" + std::to_string(lane) + ".v1",
									  payload.bytes},
				context);
			if (!result)
				return identity_error{identity_error_code::hash_failure, "hash"};
			full += result.value().hexadecimal;
		}
		return full;
	}

	identity_result<std::string> identity_service::make_id(const std::string& prefix,
														   const canonical_payload& payload,
														   collision_registry& collisions) const
	{
		if (!valid_prefix(prefix))
			return identity_error{identity_error_code::invalid_field_name, "prefix"};
		auto digest = fingerprint(payload);
		if (!digest)
			return identity_error{digest.error().code, digest.error().field};
		const auto stable_id = prefix + '_' + digest.value();
		auto collision = collisions.check_and_register(stable_id, hexadecimal(payload.bytes));
		if (!collision)
			return identity_error{collision.error().code, collision.error().field};
		return stable_id;
	}

	identity_result<file_id>
	identity_service::make_file_id(const std::string_view project_relative_key,
								   collision_registry& collisions) const
	{
		canonical_encoder encoder{"cxxlens.file-id.v1", {1U, 1U}};
		encoder.string_field("source_key",
							 std::string{project_relative_key},
							 identity_field_role::project_relative_source);
		auto payload = encoder.finish();
		if (!payload)
			return identity_error{payload.error().code, payload.error().field};
		auto id = make_id("file", payload.value(), collisions);
		if (!id)
			return identity_error{id.error().code, id.error().field};
		return file_id{id.value()};
	}

	std::string hexadecimal(const std::span<const std::byte> bytes)
	{
		std::ostringstream output;
		output << std::hex << std::setfill('0');
		for (const auto byte : bytes)
			output << std::setw(2) << std::to_integer<unsigned>(byte);
		return output.str();
	}
} // namespace cxxlens::detail::identity
