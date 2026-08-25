#include "provider_task_v4_authority.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "provider_task_v4_authority_internal.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using identity = provider_task_v4_authority_identity;

		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {
				"materialization.task-v4-authority-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error limit_error(std::string field, std::string detail = {})
		{
			return {"materialization.task-v4-authority-limit", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error binding_error(std::string field, std::string detail = {})
		{
			return {"materialization.task-v4-authority-binding-mismatch",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error downgrade_error(std::string field, std::string detail = {})
		{
			return {
				"materialization.task-v4-authority-downgrade", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error replay_error()
		{
			return {"materialization.task-v4-authority-replay", "authority", "consumed-or-moved"};
		}

		[[nodiscard]] sdk::error overflow_error(std::string field)
		{
			return {"materialization.task-v4-authority-overflow", std::move(field), "uint64"};
		}

		[[nodiscard]] bool lower_hex(const std::string_view value) noexcept
		{
			if (value.empty())
				return false;
			for (const auto byte : value)
				if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
					return false;
			return true;
		}

		[[nodiscard]] bool typed_digest(const std::string_view value,
										const std::string_view prefix) noexcept
		{
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				lower_hex(value.substr(prefix.size()));
		}

		[[nodiscard]] bool content_digest(const std::string_view value) noexcept
		{
			return typed_digest(value, "sha256:");
		}

		[[nodiscard]] bool semantic_digest(const std::string_view value) noexcept
		{
			return typed_digest(value, "semantic-v2:sha256:");
		}

		[[nodiscard]] bool content_or_semantic_digest(const std::string_view value) noexcept
		{
			return content_digest(value) || semantic_digest(value);
		}

		[[nodiscard]] bool task_id(const std::string_view value) noexcept
		{
			return typed_digest(value, "task:semantic-v2:sha256:");
		}

		[[nodiscard]] bool session_id(const std::string_view value) noexcept
		{
			return typed_digest(value, "provider-session:sha256:");
		}

		[[nodiscard]] bool closure_id(const std::string_view value) noexcept
		{
			return typed_digest(value, "source-closure:semantic-v2:sha256:");
		}

		[[nodiscard]] bool process_binding_digest(const std::string_view value) noexcept
		{
			return typed_digest(value, "process-channel:sha256:");
		}

		[[nodiscard]] sdk::result<void> strong(const std::string_view value,
											   const std::string_view field)
		{
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return sdk::unexpected(invalid(std::string{field}, "strong-id"));
			return {};
		}

		[[nodiscard]] sdk::result<void> logical_path(const std::string_view value,
													 const std::string_view field,
													 const std::uint64_t maximum_bytes)
		{
			if (value.size() < 11U || value.size() > maximum_bytes ||
				!value.starts_with("project://") || value.ends_with('/') ||
				value.find('\0') != std::string_view::npos ||
				value.find("//", 10U) != std::string_view::npos ||
				value.find("/../") != std::string_view::npos ||
				value.find("/./") != std::string_view::npos || value.starts_with("project:///") ||
				value.ends_with("/..") || value.ends_with("/.") ||
				value.find('?') != std::string_view::npos ||
				value.find('#') != std::string_view::npos)
				return sdk::unexpected(invalid(std::string{field}, "logical-path"));
			if (auto valid = sdk::validate_utf8_text(value); !valid)
				return sdk::unexpected(invalid(std::string{field}, "utf8"));
			return {};
		}

		[[nodiscard]] bool checked_add(const std::uint64_t left,
									   const std::uint64_t right,
									   std::uint64_t& output) noexcept
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return false;
			output = left + right;
			return true;
		}

		[[nodiscard]] bool checked_mul(const std::uint64_t left,
									   const std::uint64_t right,
									   std::uint64_t& output) noexcept
		{
			if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
				return false;
			output = left * right;
			return true;
		}

		[[nodiscard]] sdk::result<void> positive_bound(const std::uint64_t value,
													   const std::uint64_t maximum,
													   const std::string_view field)
		{
			if (value == 0U || value > maximum)
				return sdk::unexpected(limit_error(std::string{field}, "outside-bound"));
			return {};
		}

		[[nodiscard]] sdk::result<void> optional_sysroot(const std::optional<std::string>& value)
		{
			if (value && value->empty())
				return sdk::unexpected(invalid("toolchain_sysroot", "empty-present-value"));
			if (value)
				return strong(*value, "toolchain_sysroot");
			return {};
		}

		[[nodiscard]] sdk::result<std::string> derive_process_binding_digest(const identity& value)
		{
			return sdk::canonical_identity_digest(
				"process-channel",
				std::array{
					sdk::canonical_value::from_string(value.process_mode),
					sdk::canonical_value::from_string(value.task_id),
					sdk::canonical_value::from_string(value.session_id),
					sdk::canonical_value::from_string(value.task_v4_digest),
					sdk::canonical_value::from_string(value.closure_id),
					sdk::canonical_value::from_string(value.closure_digest),
					sdk::canonical_value::from_string(value.manifest_digest),
					sdk::canonical_value::from_string(value.transfer_digest),
					sdk::canonical_value::from_string(std::to_string(value.stream_id)),
					sdk::canonical_value::from_string(std::to_string(value.first_sequence)),
					sdk::canonical_value::from_integer(value.read_descriptor),
					sdk::canonical_value::from_integer(value.write_descriptor),
					sdk::canonical_value::from_string(std::to_string(value.read_device)),
					sdk::canonical_value::from_string(std::to_string(value.read_inode)),
					sdk::canonical_value::from_string(std::to_string(value.read_mode)),
					sdk::canonical_value::from_string(std::to_string(value.write_device)),
					sdk::canonical_value::from_string(std::to_string(value.write_inode)),
					sdk::canonical_value::from_string(std::to_string(value.write_mode)),
				});
		}

		[[nodiscard]] sdk::canonical_value text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::canonical_value number(const std::uint64_t value)
		{
			// uint64 values are encoded as decimal text because canonical_value's integer kind is
			// signed while the wire contract defines these fields as uint64.
			return text(std::to_string(value));
		}

		[[nodiscard]] sdk::result<std::string> derive_authority_digest(const identity& value)
		{
			std::vector<sdk::canonical_value> features;
			features.reserve(value.required_features.size());
			for (const auto& feature : value.required_features)
				features.push_back(text(feature));

			std::vector<sdk::canonical_value> fields;
			fields.reserve(64U);
			const auto add_text = [&](const std::string& field)
			{
				fields.push_back(text(field));
			};
			const auto add_number = [&](const std::uint64_t field)
			{
				fields.push_back(number(field));
			};
			add_text(value.authority_schema);
			add_text(value.request_schema);
			add_text(value.request_version);
			add_text(value.request_id);
			add_text(value.request_digest);
			add_text(value.materialization_request_id);
			add_text(value.semantic_request_digest);
			add_number(value.protocol_major);
			add_number(value.protocol_minor);
			fields.push_back(sdk::canonical_value::from_tuple(std::move(features)));
			add_number(value.task_count);
			add_number(value.task_index);
			add_text(value.provider_task_id);
			add_text(value.provider_execution_id);
			add_text(value.task_schema);
			add_text(value.task_id);
			add_text(value.task_v4_digest);
			add_text(value.base_task_digest);
			add_text(value.main_logical_path);
			add_text(value.logical_working_directory);
			add_text(value.task_input_digest);
			add_text(value.normalized_invocation_digest);
			add_text(value.environment_digest);
			add_text(value.session_id);
			add_text(value.closure_id);
			add_text(value.closure_digest);
			add_text(value.manifest_digest);
			add_text(value.transfer_digest);
			add_text(value.process_mode);
			add_number(value.stream_id);
			add_number(value.first_sequence);
			add_text(value.process_binding_digest);
			fields.push_back(sdk::canonical_value::from_integer(value.read_descriptor));
			fields.push_back(sdk::canonical_value::from_integer(value.write_descriptor));
			add_number(value.read_device);
			add_number(value.read_inode);
			add_number(value.read_mode);
			add_number(value.write_device);
			add_number(value.write_inode);
			add_number(value.write_mode);
			add_text(value.toolchain_digest);
			add_text(value.toolchain_family);
			add_text(value.toolchain_exact_version);
			add_text(value.toolchain_target_triple);
			add_text(value.toolchain_executable);
			add_text(value.toolchain_executable_digest);
			add_text(value.builtin_headers_digest);
			if (value.toolchain_sysroot)
				add_text(*value.toolchain_sysroot);
			else
				fields.push_back(sdk::canonical_value::null());
			add_text(value.abi_digest);
			add_text(value.plugin_spec_digest);
			add_number(value.argument_count);
			add_number(value.longest_argument_bytes);
			add_number(value.root_count);
			add_number(value.longest_root_path_bytes);
			add_number(value.manifest_bytes);
			add_number(value.member_count);
			add_number(value.blob_count);
			add_number(value.blob_bytes);
			add_number(value.unique_blob_bytes);
			add_number(value.source_bytes);
			add_number(value.aggregate_source_bytes);
			add_number(value.output_group_count);
			add_number(value.output_bytes);
			add_number(value.resident_bytes);

			auto encoded =
				sdk::canonical_binary(sdk::canonical_value::from_tuple(std::move(fields)));
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			std::string bytes;
			bytes.reserve(encoded->size());
			for (const auto byte : *encoded)
				bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
			return sdk::semantic_digest(provider_task_v4_authority_domain, bytes);
		}

		[[nodiscard]] sdk::result<void>
		validate_identity_impl(const identity& value,
							   const provider_task_v4_authority_limits limits)
		{
			if (auto valid = limits.validate(); !valid)
				return valid;

			std::uint64_t arithmetic{};
			if (!checked_add(value.member_count, value.blob_count, arithmetic))
				return sdk::unexpected(overflow_error("member_count+blob_count"));
			if (!checked_add(value.manifest_bytes, value.blob_bytes, arithmetic))
				return sdk::unexpected(overflow_error("manifest_bytes+blob_bytes"));
			if (!checked_add(value.source_bytes, value.aggregate_source_bytes, arithmetic))
				return sdk::unexpected(overflow_error("source_bytes+aggregate_source_bytes"));
			if (!checked_mul(value.blob_count, limits.maximum_blob_bytes, arithmetic))
				return sdk::unexpected(overflow_error("blob_count*maximum_blob_bytes"));
			const auto maximum_blob_total = arithmetic;

			if (value.request_schema != provider_task_v4_authority_request_schema ||
				value.request_version != provider_task_v4_authority_request_version)
				return sdk::unexpected(downgrade_error("request_version", "request-v2.2-required"));
			if (value.authority_schema != provider_task_v4_authority_schema)
				return sdk::unexpected(invalid("authority_schema", "unsupported"));
			if (value.protocol_major != 2U || value.protocol_minor != 0U)
				return sdk::unexpected(
					downgrade_error("protocol", "protocol-2.0-exact-no-downgrade"));
			if (value.required_features.size() != 2U ||
				value.required_features[0] != "task-input-chunks-v2" ||
				value.required_features[1] != "task-source-closure-v2")
				return sdk::unexpected(
					downgrade_error("required_features", "exact-v2-capabilities"));

			if (!semantic_digest(value.request_digest) ||
				value.request_id != "materialization-request:" + value.request_digest)
				return sdk::unexpected(binding_error("request_id", "request-digest"));
			if (!strong(value.materialization_request_id, "materialization_request_id"))
				return sdk::unexpected(invalid("materialization_request_id", "strong-id"));
			if (!semantic_digest(value.semantic_request_digest))
				return sdk::unexpected(invalid("semantic_request_digest", "semantic-digest"));

			if (auto valid = positive_bound(value.task_count, limits.maximum_tasks, "task_count");
				!valid)
				return valid;
			if (value.task_index >= value.task_count)
				return sdk::unexpected(binding_error("task_index", "outside-request"));
			if (value.task_schema != provider_task_v4_authority_task_schema ||
				!task_id(value.provider_task_id) || !task_id(value.task_id) ||
				!semantic_digest(value.task_v4_digest) ||
				value.task_id != "task:" + value.task_v4_digest ||
				value.provider_task_id != value.task_id)
				return sdk::unexpected(binding_error("task", "task-v4-identity"));
			if (!content_digest(value.base_task_digest))
				return sdk::unexpected(invalid("base_task_digest", "content-digest"));
			if (auto valid = logical_path(value.main_logical_path, "main_logical_path", 4096U);
				!valid)
				return valid;
			if (auto valid = logical_path(
					value.logical_working_directory, "logical_working_directory", 4096U);
				!valid)
				return valid;
			if (!strong(value.provider_execution_id, "provider_execution_id"))
				return sdk::unexpected(invalid("provider_execution_id", "strong-id"));
			if (!content_digest(value.task_input_digest) ||
				!semantic_digest(value.normalized_invocation_digest) ||
				!content_digest(value.environment_digest))
				return sdk::unexpected(invalid("task.open_task", "digest"));

			if (!session_id(value.session_id) || !closure_id(value.closure_id) ||
				!semantic_digest(value.closure_digest) ||
				value.closure_id != "source-closure:" + value.closure_digest ||
				!semantic_digest(value.manifest_digest) || !semantic_digest(value.transfer_digest))
				return sdk::unexpected(binding_error("closure", "source-closure-identity"));

			if (value.process_mode != provider_task_v4_authority_process_mode ||
				value.stream_id == 0U || value.first_sequence != 0U ||
				!process_binding_digest(value.process_binding_digest))
				return sdk::unexpected(
					invalid("process", "mode-stream-sequence-or-binding-digest"));
			if (value.read_descriptor < 4 || value.write_descriptor < 4 ||
				value.read_descriptor == value.write_descriptor || value.read_device == 0U ||
				value.read_inode == 0U || value.read_mode == 0U || value.write_device == 0U ||
				value.write_inode == 0U || value.write_mode == 0U)
				return sdk::unexpected(invalid("process", "endpoint-identity"));
			auto expected_process = derive_process_binding_digest(value);
			if (!expected_process || *expected_process != value.process_binding_digest)
				return sdk::unexpected(
					binding_error("process_binding_digest", "canonical-identity"));

			if (!content_or_semantic_digest(value.toolchain_digest) ||
				!strong(value.toolchain_family, "toolchain_family") ||
				!strong(value.toolchain_exact_version, "toolchain_exact_version") ||
				!strong(value.toolchain_target_triple, "toolchain_target_triple") ||
				!strong(value.toolchain_executable, "toolchain_executable") ||
				!content_digest(value.toolchain_executable_digest) ||
				!content_digest(value.builtin_headers_digest) ||
				!content_digest(value.abi_digest) || !content_digest(value.plugin_spec_digest))
				return sdk::unexpected(invalid("toolchain", "identity"));
			if (auto valid = optional_sysroot(value.toolchain_sysroot); !valid)
				return valid;

			if (auto valid = positive_bound(
					value.argument_count, limits.maximum_arguments, "argument_count");
				!valid)
				return valid;
			if (auto valid = positive_bound(value.longest_argument_bytes,
											limits.maximum_argument_bytes,
											"longest_argument_bytes");
				!valid)
				return valid;
			if (auto valid = positive_bound(value.root_count, limits.maximum_roots, "root_count");
				!valid)
				return valid;
			if (auto valid = positive_bound(value.longest_root_path_bytes,
											limits.maximum_root_path_bytes,
											"longest_root_path_bytes");
				!valid)
				return valid;
			if (auto valid = positive_bound(
					value.manifest_bytes, limits.maximum_manifest_bytes, "manifest_bytes");
				!valid)
				return valid;
			if (auto valid =
					positive_bound(value.member_count, limits.maximum_members, "member_count");
				!valid)
				return valid;
			if (auto valid = positive_bound(value.blob_count, limits.maximum_blobs, "blob_count");
				!valid)
				return valid;
			if (value.blob_bytes == 0U || value.blob_bytes > limits.maximum_unique_blob_bytes ||
				value.blob_bytes > maximum_blob_total)
				return sdk::unexpected(limit_error("blob_bytes", "outside-bound"));
			if (value.unique_blob_bytes == 0U ||
				value.unique_blob_bytes > limits.maximum_unique_blob_bytes ||
				value.blob_bytes > value.unique_blob_bytes)
				return sdk::unexpected(limit_error("unique_blob_bytes", "outside-bound"));
			if (auto valid =
					positive_bound(value.source_bytes, limits.maximum_source_bytes, "source_bytes");
				!valid)
				return valid;
			if (auto valid = positive_bound(value.aggregate_source_bytes,
											limits.maximum_aggregate_source_bytes,
											"aggregate_source_bytes");
				!valid)
				return valid;
			if (value.source_bytes > value.aggregate_source_bytes)
				return sdk::unexpected(binding_error("source_bytes", "aggregate-source-bound"));
			if (auto valid = positive_bound(
					value.output_group_count, limits.maximum_output_groups, "output_group_count");
				!valid)
				return valid;
			if (value.output_bytes > limits.maximum_output_bytes ||
				value.resident_bytes > limits.maximum_resident_bytes)
				return sdk::unexpected(limit_error("output-or-resident-bytes", "outside-bound"));
			return {};
		}
	} // namespace

	sdk::result<void> provider_task_v4_authority_limits::validate() const
	{
		const auto check = [](const std::uint64_t value,
							  const std::uint64_t hard,
							  const std::string_view field) -> sdk::result<void>
		{
			if (value == 0U || value > hard)
				return sdk::unexpected(limit_error(std::string{field}, "hard-bound"));
			return {};
		};
		if (auto valid = check(maximum_tasks, 4096U, "maximum_tasks"); !valid)
			return valid;
		if (auto valid = check(maximum_arguments, 4096U, "maximum_arguments"); !valid)
			return valid;
		if (auto valid = check(maximum_argument_bytes, 2048U, "maximum_argument_bytes"); !valid)
			return valid;
		if (auto valid = check(maximum_roots, 256U, "maximum_roots"); !valid)
			return valid;
		if (auto valid = check(maximum_root_path_bytes, 4096U, "maximum_root_path_bytes"); !valid)
			return valid;
		if (auto valid =
				check(maximum_manifest_bytes, 40U * 1024U * 1024U, "maximum_manifest_bytes");
			!valid)
			return valid;
		if (auto valid = check(maximum_members, 4096U, "maximum_members"); !valid)
			return valid;
		if (auto valid = check(maximum_blobs, 4096U, "maximum_blobs"); !valid)
			return valid;
		if (auto valid = check(maximum_blob_bytes, 16U * 1024U * 1024U, "maximum_blob_bytes");
			!valid)
			return valid;
		if (auto valid =
				check(maximum_unique_blob_bytes, 48U * 1024U * 1024U, "maximum_unique_blob_bytes");
			!valid)
			return valid;
		if (maximum_blob_bytes > maximum_unique_blob_bytes)
			return sdk::unexpected(limit_error("maximum_blob_bytes", "unique-blob-order"));
		if (auto valid = check(maximum_source_bytes, 16U * 1024U * 1024U, "maximum_source_bytes");
			!valid)
			return valid;
		if (auto valid = check(maximum_aggregate_source_bytes,
							   512U * 1024U * 1024U,
							   "maximum_aggregate_source_bytes");
			!valid)
			return valid;
		if (maximum_source_bytes > maximum_aggregate_source_bytes)
			return sdk::unexpected(limit_error("maximum_source_bytes", "aggregate-source-order"));
		if (auto valid = check(maximum_output_groups, 4096U, "maximum_output_groups"); !valid)
			return valid;
		if (auto valid = check(maximum_output_bytes, 1024U * 1024U * 1024U, "maximum_output_bytes");
			!valid)
			return valid;
		if (auto valid = check(maximum_resident_bytes, 1'310'720U, "maximum_resident_bytes");
			!valid)
			return valid;
		return {};
	}

	sdk::result<void>
	validate_provider_task_v4_authority_identity(const provider_task_v4_authority_identity& value,
												 const provider_task_v4_authority_limits limits)
	{
		try
		{
			return validate_identity_impl(value, limits);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(invalid("identity", "allocation"));
		}
	}

	sdk::result<std::string>
	derive_provider_task_v4_authority_digest(const provider_task_v4_authority_identity& value)
	{
		try
		{
			if (auto valid = validate_provider_task_v4_authority_identity(value); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return derive_authority_digest(value);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(invalid("identity", "allocation"));
		}
	}

	struct provider_task_v4_authority::state
	{
		identity value;
		std::string digest;
		bool live{true};
	};

	provider_task_v4_authority::provider_task_v4_authority(std::unique_ptr<state> state) noexcept
		: state_{std::move(state)}
	{
	}

	provider_task_v4_authority::~provider_task_v4_authority() = default;

	provider_task_v4_authority::provider_task_v4_authority(provider_task_v4_authority&&) noexcept =
		default;

	provider_task_v4_authority&
	provider_task_v4_authority::operator=(provider_task_v4_authority&&) noexcept = default;

	bool provider_task_v4_authority::valid() const noexcept
	{
		return state_ != nullptr && state_->live;
	}

	std::string_view provider_task_v4_authority::authority_digest() const noexcept
	{
		return valid() ? std::string_view{state_->digest} : std::string_view{};
	}

	sdk::result<provider_task_v4_authority_identity> provider_task_v4_authority::snapshot() const
	{
		if (!valid())
			return sdk::unexpected(replay_error());
		try
		{
			return state_->value;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(invalid("snapshot", "allocation"));
		}
	}

	sdk::result<provider_task_v4_authority_identity> provider_task_v4_authority::consume() &&
	{
		if (!valid())
			return sdk::unexpected(replay_error());
		try
		{
			state_->live = false;
			auto value = std::move(state_->value);
			state_.reset();
			return value;
		}
		catch (const std::bad_alloc&)
		{
			state_.reset();
			return sdk::unexpected(invalid("consume", "allocation"));
		}
	}

	bool provider_task_v4_authority::matches(
		const provider_task_v4_authority_identity& candidate) const noexcept
	{
		return valid() && state_->value == candidate;
	}

	sdk::result<provider_task_v4_authority>
	issue_provider_task_v4_authority(provider_task_v4_authority_identity&& value,
									 const provider_task_v4_authority_limits limits)
	{
		try
		{
			if (auto valid = validate_provider_task_v4_authority_identity(value, limits); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto digest = derive_authority_digest(value);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			auto state = std::make_unique<provider_task_v4_authority::state>();
			state->value = std::move(value);
			state->digest = std::move(*digest);
			return provider_task_v4_authority{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(invalid("issue", "allocation"));
		}
	}
} // namespace cxxlens::detail::clang22
