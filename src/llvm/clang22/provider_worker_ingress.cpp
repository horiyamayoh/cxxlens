#include "provider_worker_ingress.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <istream>
#include <map>
#include <new>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>

#include "materialization_json.hpp"
#include "observation_v2.hpp"
#include "protocol_v2/codec.hpp"
#include "provider_worker_protocol_v2_input.hpp"
#include "sdk/provider_protocol_v2_adapter.hpp"
#include "source_closure_task_v4.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using materialization::json_value;

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<const json_value*>
		member(const json_value& object, const std::string_view name, const std::string_view field)
		{
			if (object.as_object() == nullptr)
				return sdk::unexpected(failure(
					"provider.worker-v4-input-invalid", std::string{field}, "object-required"));
			const auto* value = object.member(name);
			if (value == nullptr)
				return sdk::unexpected(failure("provider.worker-v4-input-invalid",
											   std::string{field},
											   "missing:" + std::string{name}));
			return value;
		}

		[[nodiscard]] sdk::result<std::string> string_member(const json_value& object,
															 const std::string_view name,
															 const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (const auto* string = (*value)->as_string())
				return *string;
			return sdk::unexpected(
				failure("provider.worker-v4-input-invalid", std::string{field}, "string-required"));
		}

		[[nodiscard]] sdk::result<std::uint64_t> unsigned_member(const json_value& object,
																 const std::string_view name,
																 const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (const auto* unsigned_value = (*value)->as_unsigned_integer())
				return *unsigned_value;
			if (const auto* signed_value = (*value)->as_signed_integer();
				signed_value != nullptr && *signed_value >= 0)
				return static_cast<std::uint64_t>(*signed_value);
			return sdk::unexpected(failure("provider.worker-v4-input-invalid",
										   std::string{field},
										   "unsigned-integer-required"));
		}

		[[nodiscard]] sdk::result<void>
		exact_members(const json_value& object,
					  const std::span<const std::string_view> expected,
					  const std::string_view field)
		{
			if (object.as_object() == nullptr || !object.has_exact_members(expected))
				return sdk::unexpected(failure(
					"provider.worker-v4-input-invalid", std::string{field}, "field-census"));
			return {};
		}

		[[nodiscard]] sdk::result<std::vector<std::string>> string_array_member(
			const json_value& object, const std::string_view name, const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			const auto* array = (*value)->as_array();
			if (array == nullptr || array->empty() || array->size() > 4096U)
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", std::string{field}, "array-bound"));
			std::vector<std::string> output;
			output.reserve(array->size());
			for (const auto& item : *array)
			{
				const auto* string = item.as_string();
				if (string == nullptr || string->empty() || string->size() > 4096U ||
					string->find('\0') != std::string::npos)
					return sdk::unexpected(failure(
						"provider.worker-v4-input-invalid", std::string{field}, "string-bound"));
				output.push_back(*string);
			}
			return output;
		}

		[[nodiscard]] bool contains_forbidden_source_bytes(const json_value& value)
		{
			if (const auto* object = value.as_object(); object != nullptr)
			{
				for (const auto& [name, child] : *object)
				{
					if (name == "content_base64" || name == "source_bytes" ||
						name == "source_bytes_base64")
						return true;
					if (contains_forbidden_source_bytes(child))
						return true;
				}
			}
			else if (const auto* array = value.as_array(); array != nullptr)
				return std::ranges::any_of(*array, contains_forbidden_source_bytes);
			return false;
		}

		[[nodiscard]] bool lower_hex(const std::string_view value) noexcept
		{
			return !value.empty() &&
				std::ranges::all_of(value,
									[](const unsigned char byte)
									{
										return (byte >= static_cast<unsigned char>('0') &&
												byte <= static_cast<unsigned char>('9')) ||
											(byte >= static_cast<unsigned char>('a') &&
											 byte <= static_cast<unsigned char>('f'));
									});
		}

		[[nodiscard]] bool typed_digest(const std::string_view value,
										const std::string_view prefix) noexcept
		{
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				lower_hex(value.substr(prefix.size()));
		}

		[[nodiscard]] sdk::result<void>
		validate_binding_grammar(const source_closure_transfer_binding& binding,
								 const std::string_view expected_transfer)
		{
			if (!typed_digest(binding.session_id, "provider-session:sha256:") ||
				!typed_digest(binding.task_v4_digest, "semantic-v2:sha256:") ||
				binding.task_id != "task:" + binding.task_v4_digest ||
				!typed_digest(binding.closure_digest, "semantic-v2:sha256:") ||
				!typed_digest(binding.closure_id, "source-closure:semantic-v2:sha256:") ||
				binding.closure_id != "source-closure:" + binding.closure_digest ||
				!typed_digest(binding.manifest_digest, "semantic-v2:sha256:") ||
				!typed_digest(expected_transfer, "semantic-v2:sha256:") ||
				binding.first_sequence != 0U)
				return sdk::unexpected(
					failure("source-closure.task-binding-mismatch", "closure-binding", "identity"));
			return {};
		}

		[[nodiscard]] sdk::result<provider_worker_v4_ingress>
		decode_document(const json_value& root)
		{
			constexpr std::array<std::string_view, 9U> root_fields{"base_task_projection",
																   "closure_binding",
																   "expected_base_task_digest",
																   "expected_task_v4_input_digest",
																   "input_authority",
																   "output_authority",
																   "stream_id",
																   "task_v4_payload",
																   "schema"};
			if (auto valid = exact_members(root, root_fields, "envelope"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto schema = string_member(root, "schema", "schema");
			if (!schema || *schema != "cxxlens.clang22.worker-ingress.v4")
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "schema", "unsupported"));
			auto base_digest = string_member(root, "expected_base_task_digest", "base-task-digest");
			auto input_digest =
				string_member(root, "expected_task_v4_input_digest", "task-v4-input-digest");
			auto stream_id = unsigned_member(root, "stream_id", "stream-id");
			if (!base_digest || !input_digest || !stream_id || *stream_id == 0U ||
				!typed_digest(*base_digest, "sha256:") || !typed_digest(*input_digest, "sha256:"))
				return sdk::unexpected(failure("provider.worker-v4-input-invalid", "authority"));

			auto closure_object = member(root, "closure_binding", "closure-binding");
			auto authority_object = member(root, "input_authority", "input-authority");
			auto output_object = member(root, "output_authority", "output-authority");
			auto payload_object = member(root, "task_v4_payload", "task-v4-payload");
			auto base_projection_object =
				member(root, "base_task_projection", "base-task-projection");
			if (!closure_object || !authority_object || !output_object || !payload_object ||
				!base_projection_object || (*closure_object)->as_object() == nullptr ||
				(*authority_object)->as_object() == nullptr ||
				(*output_object)->as_object() == nullptr ||
				(*payload_object)->as_object() == nullptr ||
				(*base_projection_object)->as_object() == nullptr)
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "authority", "object"));
			if (contains_forbidden_source_bytes(**payload_object) ||
				contains_forbidden_source_bytes(**base_projection_object))
				return sdk::unexpected(failure(
					"provider.worker-v4-input-invalid", "authority", "source-bytes-forbidden"));

			constexpr std::array<std::string_view, 9U> closure_fields{"closure_digest",
																	  "closure_id",
																	  "expected_transfer_digest",
																	  "first_sequence",
																	  "manifest_digest",
																	  "session_id",
																	  "task_id",
																	  "task_v4_digest",
																	  "stream_id"};
			if (auto valid = exact_members(**closure_object, closure_fields, "closure-binding");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			constexpr std::array<std::string_view, 4U> authority_fields{
				"effective_arguments",
				"logical_working_directory",
				"normalized_invocation_digest",
				"qualified_read_roots"};
			if (auto valid = exact_members(**authority_object, authority_fields, "input-authority");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			constexpr std::array<std::string_view, 10U> output_fields{"dependency_groups",
																	  "descriptor_digests",
																	  "maximum_output_bytes",
																	  "maximum_rows",
																	  "provider_id",
																	  "provider_version",
																	  "requested_descriptor_ids",
																	  "semantic_contract_digest",
																	  "toolchain_context_id",
																	  "compile_unit_id"};
			if (auto valid = exact_members(**output_object, output_fields, "output-authority");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			constexpr std::array<std::string_view, 10U> payload_fields{"base_provider_task_id",
																	   "base_task_index",
																	   "base_task_digest",
																	   "logical_working_directory",
																	   "main_logical_path",
																	   "open_task",
																	   "schema",
																	   "source_closure",
																	   "task_id",
																	   "task_v4_digest"};
			if (auto valid = exact_members(**payload_object, payload_fields, "task-v4-payload");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			constexpr std::array<std::string_view, 4U> open_fields{"environment_digest",
																   "normalized_invocation_digest",
																   "task_input_digest",
																   "toolchain_digest"};
			auto open_object = member(**payload_object, "open_task", "task-v4.open-task");
			auto closure_reference =
				member(**payload_object, "source_closure", "task-v4.source-closure");
			if (!open_object || !closure_reference || (*open_object)->as_object() == nullptr ||
				(*closure_reference)->as_object() == nullptr)
				return sdk::unexpected(failure(
					"provider.worker-v4-input-invalid", "task-v4-payload", "nested-object"));
			if (auto valid = exact_members(**open_object, open_fields, "task-v4.open-task"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			constexpr std::array<std::string_view, 3U> closure_reference_fields{
				"digest", "id", "manifest_digest"};
			if (auto valid = exact_members(
					**closure_reference, closure_reference_fields, "task-v4.source-closure");
				!valid)
				return sdk::unexpected(std::move(valid.error()));

			auto closure_digest =
				string_member(**closure_object, "closure_digest", "closure.digest");
			auto closure_id = string_member(**closure_object, "closure_id", "closure.id");
			auto transfer_digest =
				string_member(**closure_object, "expected_transfer_digest", "closure.transfer");
			auto manifest_digest =
				string_member(**closure_object, "manifest_digest", "closure.manifest");
			auto session_id = string_member(**closure_object, "session_id", "closure.session");
			auto task_id = string_member(**closure_object, "task_id", "closure.task");
			auto task_digest =
				string_member(**closure_object, "task_v4_digest", "closure.task-digest");
			auto first_sequence =
				unsigned_member(**closure_object, "first_sequence", "closure.sequence");
			auto binding_stream = unsigned_member(**closure_object, "stream_id", "closure.stream");
			if (!closure_digest || !closure_id || !transfer_digest || !manifest_digest ||
				!session_id || !task_id || !task_digest || !first_sequence || !binding_stream ||
				*first_sequence != 0U || *binding_stream != *stream_id)
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "closure-binding"));

			source_closure_transfer_binding binding{*session_id,
													*task_id,
													*task_digest,
													*closure_id,
													*closure_digest,
													*manifest_digest,
													*first_sequence};
			if (auto valid = validate_binding_grammar(binding, *transfer_digest); !valid)
				return sdk::unexpected(std::move(valid.error()));

			auto normalized = string_member(
				**authority_object, "normalized_invocation_digest", "input-authority.invocation");
			auto working_directory = string_member(**authority_object,
												   "logical_working_directory",
												   "input-authority.working-directory");
			auto arguments = string_array_member(
				**authority_object, "effective_arguments", "input-authority.arguments");
			auto roots = string_array_member(
				**authority_object, "qualified_read_roots", "input-authority.read-roots");
			auto provider_id = string_member(**output_object, "provider_id", "output.provider-id");
			auto provider_version =
				string_member(**output_object, "provider_version", "output.provider-version");
			auto semantic_contract = string_member(
				**output_object, "semantic_contract_digest", "output.semantic-contract");
			auto toolchain_context =
				string_member(**output_object, "toolchain_context_id", "output.toolchain-context");
			auto compile_unit_id =
				string_member(**output_object, "compile_unit_id", "output.compile-unit");
			auto descriptor_ids = string_array_member(
				**output_object, "requested_descriptor_ids", "output.descriptors");
			auto descriptor_digests = string_array_member(
				**output_object, "descriptor_digests", "output.descriptor-digests");
			auto dependency_groups = string_array_member(
				**output_object, "dependency_groups", "output.dependency-groups");
			auto maximum_rows = unsigned_member(**output_object, "maximum_rows", "output.rows");
			auto maximum_output_bytes =
				unsigned_member(**output_object, "maximum_output_bytes", "output.bytes");
			if (!normalized || !working_directory || !arguments || !roots)
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "input-authority"));

			auto payload_schema = string_member(**payload_object, "schema", "task-v4.schema");
			auto payload_task_id = string_member(**payload_object, "task_id", "task-v4.task-id");
			auto payload_task_digest =
				string_member(**payload_object, "task_v4_digest", "task-v4.task-digest");
			auto payload_main =
				string_member(**payload_object, "main_logical_path", "task-v4.main-path");
			auto payload_working = string_member(
				**payload_object, "logical_working_directory", "task-v4.working-directory");
			auto payload_base_digest =
				string_member(**payload_object, "base_task_digest", "task-v4.base-digest");
			auto payload_index =
				unsigned_member(**payload_object, "base_task_index", "task-v4.index");
			auto payload_closure_digest =
				string_member(**closure_reference, "digest", "task-v4.source-closure.digest");
			auto payload_closure_id =
				string_member(**closure_reference, "id", "task-v4.source-closure.id");
			auto payload_manifest_digest = string_member(
				**closure_reference, "manifest_digest", "task-v4.source-closure.manifest");
			if (!payload_schema || *payload_schema != provider_task_v4_schema || !payload_task_id ||
				!payload_task_digest || !payload_main || !payload_working || !payload_base_digest ||
				!payload_index || !payload_closure_digest || !payload_closure_id ||
				!payload_manifest_digest || *payload_task_id != binding.task_id ||
				*payload_task_digest != binding.task_v4_digest ||
				*payload_base_digest != *base_digest || *payload_working != *working_directory ||
				*payload_closure_digest != binding.closure_digest ||
				*payload_closure_id != binding.closure_id ||
				*payload_manifest_digest != binding.manifest_digest)
				return sdk::unexpected(
					failure("source-closure.task-binding-mismatch", "task-v4-payload", "identity"));

			auto open_normalized = string_member(
				**open_object, "normalized_invocation_digest", "task-v4.open-task.invocation");
			auto open_environment =
				string_member(**open_object, "environment_digest", "task-v4.open-task.environment");
			auto open_input =
				string_member(**open_object, "task_input_digest", "task-v4.open-task.input");
			auto open_toolchain =
				string_member(**open_object, "toolchain_digest", "task-v4.open-task.toolchain");
			if (!open_normalized || !open_environment || !open_input || !open_toolchain ||
				!typed_digest(*open_normalized, "semantic-v2:sha256:") ||
				!typed_digest(*open_environment, "sha256:") ||
				!typed_digest(*open_input, "sha256:") ||
				!typed_digest(*open_toolchain, "semantic-v2:sha256:") ||
				*open_normalized != *normalized)
				return sdk::unexpected(
					failure("source-closure.task-binding-mismatch", "input-authority.invocation"));
			auto payload_text = materialization::canonical_json(**payload_object);
			if (payload_text.size() > source_closure_task_v4_maximum_payload_bytes)
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "task-v4-input-payload", "bytes"));
			auto payload_bytes = std::as_bytes(std::span{payload_text.data(), payload_text.size()});
			if (sdk::content_digest(payload_bytes) != *input_digest)
				return sdk::unexpected(failure("source-closure.task-v4-input-digest-mismatch",
											   "task_v4_input_digest",
											   "payload"));
			auto base_text = materialization::canonical_json(**base_projection_object);
			if (base_text.size() > source_closure_task_v4_maximum_payload_bytes)
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "base-task-projection", "bytes"));
			auto base_bytes = std::as_bytes(std::span{base_text.data(), base_text.size()});
			if (sdk::content_digest(base_bytes) != *base_digest)
				return sdk::unexpected(
					failure("source-closure.task-v4-binding-mismatch", "base-task-projection"));

			provider_task_v4_input_authority authority{std::move(*normalized),
													   std::move(*working_directory),
													   std::move(*arguments),
													   std::move(*roots)};
			if (auto valid = authority.validate(*payload_main, *payload_working, *open_normalized);
				!valid)
				return sdk::unexpected(std::move(valid.error()));

			provider_worker_v4_ingress::output_authority output_authority{
				provider_id ? std::move(*provider_id) : std::string{},
				provider_version ? std::move(*provider_version) : std::string{},
				semantic_contract ? std::move(*semantic_contract) : std::string{},
				toolchain_context ? std::move(*toolchain_context) : std::string{},
				compile_unit_id ? std::move(*compile_unit_id) : std::string{},
				descriptor_ids ? std::move(*descriptor_ids) : std::vector<std::string>{},
				descriptor_digests ? std::move(*descriptor_digests) : std::vector<std::string>{},
				dependency_groups ? std::move(*dependency_groups) : std::vector<std::string>{},
				maximum_rows ? *maximum_rows : 0U,
				maximum_output_bytes ? *maximum_output_bytes : 0U};
			if (!provider_id || !provider_version || !semantic_contract || !toolchain_context ||
				!compile_unit_id || !descriptor_ids || !descriptor_digests || !dependency_groups ||
				!maximum_rows || !maximum_output_bytes)
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "output-authority"));
			if (auto valid = output_authority.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));

			return provider_worker_v4_ingress{
				std::vector<std::byte>{payload_bytes.begin(), payload_bytes.end()},
				std::vector<std::byte>{base_bytes.begin(), base_bytes.end()},
				std::move(*base_digest),
				std::move(*input_digest),
				std::move(authority),
				std::move(binding),
				std::move(*transfer_digest),
				*stream_id,
				std::move(output_authority)};
		}
	} // namespace

	sdk::result<void> provider_worker_v4_ingress::output_authority::validate() const
	{
		if (!sdk::validate_strong_id(provider_id) || provider_version.empty() ||
			!typed_digest(semantic_contract_digest, "semantic-v2:sha256:") ||
			!sdk::validate_strong_id(toolchain_context_id) ||
			!sdk::validate_strong_id(compile_unit_id) || maximum_rows == 0U ||
			maximum_rows > 100000U || maximum_output_bytes == 0U ||
			maximum_output_bytes > std::uint64_t{16U} * 1024U * 1024U ||
			requested_descriptor_ids.size() != task_v4_output_descriptor_ids.size() ||
			descriptor_digests.size() != task_v4_output_descriptor_ids.size() ||
			!std::ranges::equal(requested_descriptor_ids, task_v4_output_descriptor_ids) ||
			dependency_groups.size() != task_v4_dependency_groups.size() ||
			!std::ranges::equal(dependency_groups, task_v4_dependency_groups))
			return sdk::unexpected(
				failure("provider.worker-v4.output-authority-invalid", "output-authority"));
		const std::array<const sdk::relation_descriptor*, 6U> descriptors{
			&cc::relations::call_direct_target::descriptor(),
			&cc::relations::call_site::descriptor(),
			&cc::relations::entity::descriptor(),
			&materialization::call_observation_v2_descriptor(),
			&materialization::entity_observation_v2_descriptor(),
			&materialization::type_observation_v2_descriptor()};
		for (std::size_t index{}; index < descriptors.size(); ++index)
			if (descriptor_digests.at(index) != descriptors.at(index)->descriptor_digest)
				return sdk::unexpected(
					failure("provider.worker-v4.output-authority-invalid", "descriptor-digest"));
		std::size_t dots{};
		for (const auto value : provider_version)
		{
			if (value == '.')
				++dots;
			else if (value < '0' || value > '9')
				return sdk::unexpected(
					failure("provider.worker-v4.output-authority-invalid", "provider-version"));
		}
		if (dots != 2U || provider_version.front() == '.' || provider_version.back() == '.')
			return sdk::unexpected(
				failure("provider.worker-v4.output-authority-invalid", "provider-version"));
		return {};
	}

	sdk::result<provider_worker_v4_ingress> decode_provider_worker_v4_ingress(std::string raw)
	{
		try
		{
			materialization::json_limits limits;
			limits.max_input_bytes = provider_worker_v4_maximum_envelope_bytes;
			limits.max_depth = 16U;
			limits.max_array_elements = 4096U;
			limits.max_object_members = 32U;
			limits.max_string_bytes = std::size_t{8U} * 1024U * 1024U;
			limits.max_total_string_bytes = provider_worker_v4_maximum_envelope_bytes;
			limits.max_total_values = std::size_t{256U} * 1024U;
			auto document = materialization::parse_json_object(std::move(raw), limits);
			if (!document)
				return sdk::unexpected(failure(
					"provider.worker-v4-input-invalid", "envelope", document.error().detail));
			if (materialization::canonical_json(document->root()) != document->raw_bytes())
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "envelope", "noncanonical"));
			return decode_document(document->root());
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("provider.worker-v4-input-invalid", "envelope", "allocation"));
		}
	}

	namespace
	{
		[[nodiscard]] bool protocol_v2_json_prefix(const std::byte value) noexcept
		{
			const auto byte = std::to_integer<unsigned char>(value);
			return byte == static_cast<unsigned char>('{') ||
				byte == static_cast<unsigned char>('[') ||
				byte == static_cast<unsigned char>('"') ||
				byte == static_cast<unsigned char>(' ') ||
				byte == static_cast<unsigned char>('\t') ||
				byte == static_cast<unsigned char>('\r') ||
				byte == static_cast<unsigned char>('\n');
		}

		[[nodiscard]] sdk::result<provider_worker_protocol_v2_launch_envelope>
		decode_protocol_v2_input_bytes(const std::span<const std::byte> encoded,
									   const sdk::provider::host_transcript_expectation& expected)
		{
			if (encoded.size() > provider_worker_protocol_v2_maximum_wire_bytes)
				return sdk::unexpected(failure(
					"provider.worker-protocol-v2-input-invalid", "stdin", "wire-size-limit"));
			if (encoded.empty())
				return sdk::unexpected(
					failure("provider.worker-protocol-v2-input-invalid", "stdin", "empty"));
			if (protocol_v2_json_prefix(encoded.front()))
				return sdk::unexpected(failure(
					"provider.worker-protocol-v2-input-invalid", "stdin", "json-input-forbidden"));

			// The task-input-chunks-v2 profile binds exact SHA-256 content bytes.  A task-v4
			// semantic digest is a different authority and must never be accepted as this field.
			if (!typed_digest(expected.task.task_input_digest, "sha256:"))
				return sdk::unexpected(failure("provider.worker-protocol-v2-input-invalid",
											   "task_input_digest",
											   "content-digest-required"));

			auto frames = sdk::provider::decode_frame_stream(
				encoded, expected.limits, provider_worker_protocol_v2_maximum_frames);
			if (!frames)
				return sdk::unexpected(std::move(frames.error()));
			auto validated = sdk::provider::validate_host_transcript(*frames, expected);
			if (!validated)
				return sdk::unexpected(std::move(validated.error()));

			const auto protocol_content_digest = sdk::content_digest(validated->payload);
			if (protocol_content_digest != expected.task.task_input_digest)
				return sdk::unexpected(failure("provider.worker-protocol-v2-input-invalid",
											   "protocol_content_digest",
											   "payload-mismatch"));

			return provider_worker_protocol_v2_launch_envelope{
				std::string{expected.provider_manifest},
				std::move(validated->task),
				validated->credit,
				std::move(validated->payload),
				protocol_content_digest,
				1U,
				sdk::provider::protocol_v2_major,
				sdk::provider::protocol_v2_minor};
		}
	} // namespace

	sdk::result<provider_worker_protocol_v2_launch_envelope>
	decode_provider_worker_protocol_v2_input(
		const std::span<const std::byte> encoded,
		const sdk::provider::host_transcript_expectation& expected)
	{
		try
		{
			return decode_protocol_v2_input_bytes(encoded, expected);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("provider.worker-protocol-v2-input-invalid", "stdin", "allocation"));
		}
	}

	sdk::result<provider_worker_protocol_v2_launch_envelope>
	decode_provider_worker_protocol_v2_input(
		std::istream& input, const sdk::provider::host_transcript_expectation& expected)
	{
		try
		{
			std::vector<std::byte> encoded;
			constexpr std::size_t read_buffer_bytes = std::size_t{64U} * 1024U;
			std::array<char, read_buffer_bytes> buffer{};
			for (;;)
			{
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const auto read_count = input.gcount();
				if (read_count > 0)
				{
					const auto count = static_cast<std::size_t>(read_count);
					if (count > provider_worker_protocol_v2_maximum_wire_bytes ||
						encoded.size() > provider_worker_protocol_v2_maximum_wire_bytes - count)
						return sdk::unexpected(failure("provider.worker-protocol-v2-input-invalid",
													   "stdin",
													   "wire-size-limit"));
					const auto bytes = std::as_bytes(std::span{buffer.data(), count});
					encoded.insert(encoded.end(), bytes.begin(), bytes.end());
				}
				if (input.bad())
					return sdk::unexpected(failure(
						"provider.worker-protocol-v2-input-invalid", "stdin", "read-failure"));
				if (input.eof())
					break;
				if (input.fail())
					return sdk::unexpected(failure(
						"provider.worker-protocol-v2-input-invalid", "stdin", "read-failure"));
			}
			return decode_protocol_v2_input_bytes(encoded, expected);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("provider.worker-protocol-v2-input-invalid", "stdin", "allocation"));
		}
	}

	namespace
	{
		[[nodiscard]] sdk::result<void> read_stream_exact(std::istream& input,
														  const std::span<std::byte> destination)
		{
			std::size_t offset{};
			while (offset < destination.size())
			{
				const auto count =
					input.rdbuf()->sgetn(reinterpret_cast<char*>(destination.data() + offset),
										 static_cast<std::streamsize>(destination.size() - offset));
				if (count <= 0)
					return sdk::unexpected(
						failure("provider.worker-protocol-v2-input-invalid", "stdin", "truncated"));
				offset += static_cast<std::size_t>(count);
			}
			return {};
		}
	} // namespace

	sdk::result<provider_worker_protocol_v2_launch_envelope>
	decode_provider_worker_protocol_v2_input_until_close(
		std::istream& input, const sdk::provider::host_transcript_expectation& expected)
	{
		try
		{
			std::vector<std::byte> encoded;
			encoded.reserve(std::size_t{16U} * 1024U);
			sdk::provider::protocol_limits limits = expected.limits;
			for (std::size_t frame_count{};
				 frame_count < provider_worker_protocol_v2_maximum_frames;
				 ++frame_count)
			{
				std::array<std::byte, protocol_v2::fixed_header_bytes> header{};
				if (auto read = read_stream_exact(input, header); !read)
					return sdk::unexpected(std::move(read.error()));
				auto prepared =
					sdk::provider::detail::prepare_provider_protocol_v2_frame(header, limits);
				if (!prepared)
					return sdk::unexpected(std::move(prepared.error()));
				const auto frame_bytes = protocol_v2::fixed_header_bytes +
					prepared->control_bytes() + prepared->payload_bytes();
				if (frame_bytes > provider_worker_protocol_v2_maximum_wire_bytes - encoded.size())
					return sdk::unexpected(failure(
						"provider.worker-protocol-v2-input-invalid", "stdin", "wire-size-limit"));
				std::vector<std::byte> control(prepared->control_bytes());
				std::vector<std::byte> payload(prepared->payload_bytes());
				if (auto read = read_stream_exact(input, control); !read)
					return sdk::unexpected(std::move(read.error()));
				if (auto read = read_stream_exact(input, payload); !read)
					return sdk::unexpected(std::move(read.error()));
				auto value = std::move(*prepared).finalize(std::move(control), std::move(payload));
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				auto wire =
					sdk::provider::detail::encode_provider_protocol_v2_frame(*value, limits);
				if (!wire)
					return sdk::unexpected(std::move(wire.error()));
				encoded.insert(encoded.end(), wire->begin(), wire->end());
				if (value->type == sdk::provider::message_type::close)
					return decode_provider_worker_protocol_v2_input(encoded, expected);
			}
			return sdk::unexpected(failure(
				"provider.worker-protocol-v2-input-invalid", "stdin", "close-frame-missing"));
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("provider.worker-protocol-v2-input-invalid", "stdin", "allocation"));
		}
	}
} // namespace cxxlens::detail::clang22
