#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/core/identity.hpp>

#include "../runtime/hash_port.hpp"

namespace cxxlens::detail::identity
{
	enum class identity_error_code : std::uint8_t
	{
		invalid_domain,
		missing_version,
		invalid_field_name,
		duplicate_field,
		forbidden_identity_input,
		invalid_project_relative_key,
		duplicate_map_key,
		collision,
		hash_failure,
	};

	struct identity_error
	{
		identity_error_code code{identity_error_code::invalid_domain};
		std::string field;
	};

	template <class T>
	class identity_result
	{
	  public:
		identity_result(T value) : stored_value_{std::move(value)}, has_value_{true} {}
		identity_result(identity_error error) : stored_error_{std::move(error)} {}
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value_;
		}
		[[nodiscard]] const T& value() const
		{
			return stored_value_;
		}
		[[nodiscard]] T& value()
		{
			return stored_value_;
		}
		[[nodiscard]] const identity_error& error() const
		{
			return stored_error_;
		}

	  private:
		T stored_value_{};
		identity_error stored_error_{};
		bool has_value_{};
	};

	enum class identity_field_role : std::uint8_t
	{
		semantic,
		project_relative_source,
		absolute_path,
		wall_time,
		process_id,
		thread_id,
		pointer_value,
		cache_state,
		observation_order,
		diagnostic_message,
	};

	struct canonical_payload
	{
		std::string domain;
		std::uint32_t schema_version{};
		std::uint32_t semantics_version{};
		std::vector<std::byte> bytes;
	};

	struct encoding_versions
	{
		std::uint32_t schema{};
		std::uint32_t semantics{};
	};

	class canonical_encoder
	{
	  public:
		canonical_encoder(std::string domain, encoding_versions versions);
		canonical_encoder& string_field(std::string name,
										const std::string& value,
										identity_field_role role = identity_field_role::semantic);
		canonical_encoder& unsigned_field(std::string name, std::uint64_t value);
		canonical_encoder& signed_field(std::string name, std::int64_t value);
		canonical_encoder& enum_field(std::string name, std::int64_t value);
		canonical_encoder& boolean_field(std::string name, bool value);
		canonical_encoder& bytes_field(std::string name, std::span<const std::byte> value);
		canonical_encoder& optional_string(std::string name,
										   const std::optional<std::string>& value);
		canonical_encoder& string_sequence(std::string name,
										   const std::vector<std::string>& values);
		canonical_encoder& string_set(std::string name, std::vector<std::string> values);
		canonical_encoder& string_map(std::string name,
									  std::vector<std::pair<std::string, std::string>> values);
		[[nodiscard]] identity_result<canonical_payload> finish() const;

	  private:
		void add(std::string name, std::byte type, std::vector<std::byte> value);
		void reject(identity_error_code code, std::string field);
		std::string domain_;
		std::uint32_t schema_version_{};
		std::uint32_t semantics_version_{};
		std::map<std::string, std::vector<std::byte>> fields_;
		std::optional<identity_error> error_;
	};

	class collision_registry
	{
	  public:
		[[nodiscard]] identity_result<bool> check_and_register(std::string stable_id,
															   std::string payload_fingerprint);

	  private:
		std::map<std::string, std::string> fingerprints_;
	};

	class identity_service
	{
	  public:
		explicit identity_service(const runtime::hash_port& hashes);
		[[nodiscard]] identity_result<std::string> make_id(const std::string& prefix,
														   const canonical_payload& payload,
														   collision_registry& collisions) const;
		[[nodiscard]] identity_result<file_id> make_file_id(std::string_view project_relative_key,
															collision_registry& collisions) const;

	  private:
		[[nodiscard]] identity_result<std::string>
		fingerprint(const canonical_payload& payload) const;
		const runtime::hash_port& hashes_;
	};

	[[nodiscard]] std::string hexadecimal(std::span<const std::byte> bytes);

} // namespace cxxlens::detail::identity
