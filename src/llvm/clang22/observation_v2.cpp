#include "observation_v2.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::string_view payload_domain{"cxxlens.clang22.observation-payload.v2"};
		constexpr std::string_view origin_projection_tag{"cxxlens.clang22.source-origin-chain.v2"};

		[[nodiscard]] sdk::error observation_error(std::string field, std::string detail = {})
		{
			return {"materialization.claim-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error span_error(std::string field, std::string detail = {})
		{
			return {"materialization.span-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool utf8_byte_less(const std::string_view left,
										  const std::string_view right) noexcept
		{
			return std::lexicographical_compare(left.begin(),
												left.end(),
												right.begin(),
												right.end(),
												[](const char lhs, const char rhs)
												{
													return static_cast<unsigned char>(lhs) <
														static_cast<unsigned char>(rhs);
												});
		}

		[[nodiscard]] std::vector<std::byte> exact_bytes(const std::string_view value)
		{
			const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
			return {bytes.begin(), bytes.end()};
		}

		[[nodiscard]] sdk::detached_cell cell(sdk::value_type type, sdk::scalar_value value)
		{
			return {std::move(type), sdk::cell_state::present, std::move(value), std::nullopt};
		}

		[[nodiscard]] sdk::column_descriptor column(std::string prefix,
													std::string name,
													sdk::value_type type,
													const bool required,
													sdk::column_role role)
		{
			return {std::move(prefix) + name, std::move(name), std::move(type), required, role};
		}

		void replace_all(std::string& value,
						 const std::string_view needle,
						 const std::string_view replacement)
		{
			std::size_t offset{};
			while ((offset = value.find(needle, offset)) != std::string::npos)
			{
				value.replace(offset, needle.size(), replacement);
				offset += replacement.size();
			}
		}

		[[nodiscard]] const std::string& entity_contract_canonical()
		{
			static const std::string value =
				R"cxxlens({"api_surface":"dynamic_only","claim":{"cardinality":"functional_assertion","condition_policy":"claim-envelope-required","domain_identity":{"contract":"canonical-binary-tuple-v1","projection":["frontend.clang22.entity_observation.v2.compile_unit","frontend.clang22.entity_observation.v2.semantic_key","frontend.clang22.entity_observation.v2.payload_digest","frontend.clang22.entity_observation.v2.source","frontend.clang22.entity_observation.v2.source_snapshot","frontend.clang22.entity_observation.v2.source_file","frontend.clang22.entity_observation.v2.source_begin","frontend.clang22.entity_observation.v2.source_end","frontend.clang22.entity_observation.v2.source_role","frontend.clang22.entity_observation.v2.source_read_only","frontend.clang22.entity_observation.v2.source_origin_chain"],"result_column":"frontend.clang22.entity_observation.v2.observation"},"interpretation_required":true,"key":["frontend.clang22.entity_observation.v2.observation"]},"closure":{"supported_kinds":["relation-key-enumeration"]},"columns":[{"id":"frontend.clang22.entity_observation.v2.observation","identity_role":"claim_key","name":"observation","required":true,"type":"typed_id<clang22_observation_id>"},{"id":"frontend.clang22.entity_observation.v2.compile_unit","identity_role":"authoritative_payload","name":"compile_unit","required":true,"type":"typed_id<compile_unit_id>"},{"id":"frontend.clang22.entity_observation.v2.semantic_key","identity_role":"authoritative_payload","name":"semantic_key","required":true,"semantic":"Exact UTF-8 bytes of the validated native semantic key under cxxlens.clang22.observation-native.v2.","type":"bytes"},{"id":"frontend.clang22.entity_observation.v2.payload_digest","identity_role":"authoritative_payload","name":"payload_digest","required":true,"semantic":"Semantic-v2 digest in domain cxxlens.clang22.observation-payload.v2 over the exact descriptor-bound sorted payload tuple.","type":"digest"},{"id":"frontend.clang22.entity_observation.v2.source","identity_role":"authoritative_payload","name":"source","required":false,"semantic":"Recomputed source.span identity for the complete primary span bundle.","type":"optional<typed_id<source_span_id>>"},{"id":"frontend.clang22.entity_observation.v2.source_snapshot","identity_role":"authoritative_payload","name":"source_snapshot","required":false,"semantic":"Source snapshot in the source.span identity projection.","type":"optional<typed_id<source_snapshot_id>>"},{"id":"frontend.clang22.entity_observation.v2.source_file","identity_role":"authoritative_payload","name":"source_file","required":false,"semantic":"File identity in the source.span identity projection.","type":"optional<typed_id<file_id>>"},{"id":"frontend.clang22.entity_observation.v2.source_begin","identity_role":"authoritative_payload","name":"source_begin","required":false,"semantic":"Half-open source begin byte offset in the source.span identity projection.","type":"optional<uint64>"},{"id":"frontend.clang22.entity_observation.v2.source_end","identity_role":"authoritative_payload","name":"source_end","required":false,"semantic":"Half-open source end byte offset in the source.span identity projection.","type":"optional<uint64>"},{"id":"frontend.clang22.entity_observation.v2.source_role","identity_role":"authoritative_payload","name":"source_role","required":false,"semantic":"Explicit normalization role in the source.span identity projection.","type":"optional<open_symbol<source.range-role/1>>"},{"id":"frontend.clang22.entity_observation.v2.source_read_only","identity_role":"authoritative_payload","name":"source_read_only","required":false,"semantic":"Read-only source policy preserved in the materialized source.span payload.","type":"optional<bool>"},{"id":"frontend.clang22.entity_observation.v2.source_origin_chain","identity_role":"authoritative_payload","name":"source_origin_chain","required":false,"semantic":"Exact cxxlens-canonical-tuple-v1 bytes for the nonempty immediate-to-outermost cxxlens.clang22.source-origin-chain.v2 projection; absent for an empty chain.","type":"optional<bytes>"},{"id":"frontend.clang22.entity_observation.v2.exact_equivalence","identity_role":"authoritative_payload","name":"exact_equivalence","required":true,"type":"bool"},{"id":"frontend.clang22.entity_observation.v2.limitation","identity_role":"authoritative_payload","name":"limitation","required":false,"type":"optional<utf8_string>"}],"coverage":{"execution_domain":"frontend.clang22.entity-observation.compile-unit"},"descriptor_id":"frontend.clang22.entity_observation.v2","evolution_policy":"ng0.additive.v1","generated_cpp_tag":null,"indexes":[["frontend.clang22.entity_observation.v2.semantic_key"],["frontend.clang22.entity_observation.v2.source"]],"merge":{"conflict_columns":["frontend.clang22.entity_observation.v2.compile_unit","frontend.clang22.entity_observation.v2.exact_equivalence","frontend.clang22.entity_observation.v2.limitation","frontend.clang22.entity_observation.v2.payload_digest","frontend.clang22.entity_observation.v2.semantic_key","frontend.clang22.entity_observation.v2.source","frontend.clang22.entity_observation.v2.source_begin","frontend.clang22.entity_observation.v2.source_end","frontend.clang22.entity_observation.v2.source_file","frontend.clang22.entity_observation.v2.source_origin_chain","frontend.clang22.entity_observation.v2.source_read_only","frontend.clang22.entity_observation.v2.source_role","frontend.clang22.entity_observation.v2.source_snapshot"],"mode":"functional_assertion"},"name":"frontend.clang22.entity_observation","owner_namespace":"cxxlens.clang22.reference","partition":{"condition_fragment":"envelope","interpretation_domain":"envelope","suggested_keys":["frontend.clang22.entity_observation.v2.compile_unit"]},"profile":"NG0","provenance":{"minimum":"direct_observation"},"references":[{"on_missing":"reject_batch","source_columns":["frontend.clang22.entity_observation.v2.compile_unit"],"strength":"hard","target_columns":["build.compile_unit.v1.compile_unit"],"target_relation":"build.compile_unit"},{"on_missing":"reject_batch","source_columns":["frontend.clang22.entity_observation.v2.source","frontend.clang22.entity_observation.v2.source_snapshot","frontend.clang22.entity_observation.v2.source_file","frontend.clang22.entity_observation.v2.source_begin","frontend.clang22.entity_observation.v2.source_end","frontend.clang22.entity_observation.v2.source_role","frontend.clang22.entity_observation.v2.source_read_only"],"strength":"hard","target_columns":["source.span.v1.span","source.span.v1.snapshot","source.span.v1.file","source.span.v1.begin","source.span.v1.end","source.span.v1.role","source.span.v1.read_only"],"target_relation":"source.span"}],"row_constraints":{"all_or_none":[["frontend.clang22.entity_observation.v2.source","frontend.clang22.entity_observation.v2.source_begin","frontend.clang22.entity_observation.v2.source_end","frontend.clang22.entity_observation.v2.source_file","frontend.clang22.entity_observation.v2.source_read_only","frontend.clang22.entity_observation.v2.source_role","frontend.clang22.entity_observation.v2.source_snapshot"]]},"semantic_major":2,"semantics":"frontend.clang22.entity_observation/2","stability":"versioned","summary":"Provider-owned Clang 22 entity occurrence with revalidatable primary source authority.","version":"2.0.0"})cxxlens";
			return value;
		}

		[[nodiscard]] const std::string& type_contract_canonical()
		{
			static const std::string value =
				R"cxxlens({"api_surface":"dynamic_only","claim":{"cardinality":"functional_assertion","condition_policy":"claim-envelope-required","domain_identity":{"contract":"canonical-binary-tuple-v1","projection":["frontend.clang22.type_observation.v2.compile_unit","frontend.clang22.type_observation.v2.semantic_key","frontend.clang22.type_observation.v2.payload_digest"],"result_column":"frontend.clang22.type_observation.v2.observation"},"interpretation_required":true,"key":["frontend.clang22.type_observation.v2.observation"]},"closure":{"supported_kinds":["relation-key-enumeration"]},"columns":[{"id":"frontend.clang22.type_observation.v2.observation","identity_role":"claim_key","name":"observation","required":true,"type":"typed_id<clang22_observation_id>"},{"id":"frontend.clang22.type_observation.v2.compile_unit","identity_role":"authoritative_payload","name":"compile_unit","required":true,"type":"typed_id<compile_unit_id>"},{"id":"frontend.clang22.type_observation.v2.semantic_key","identity_role":"authoritative_payload","name":"semantic_key","required":true,"semantic":"Exact UTF-8 bytes of the validated native semantic key under cxxlens.clang22.observation-native.v2.","type":"bytes"},{"id":"frontend.clang22.type_observation.v2.payload_digest","identity_role":"authoritative_payload","name":"payload_digest","required":true,"semantic":"Semantic-v2 digest in domain cxxlens.clang22.observation-payload.v2 over the exact descriptor-bound sorted payload tuple.","type":"digest"},{"id":"frontend.clang22.type_observation.v2.exact_equivalence","identity_role":"authoritative_payload","name":"exact_equivalence","required":true,"type":"bool"},{"id":"frontend.clang22.type_observation.v2.limitation","identity_role":"authoritative_payload","name":"limitation","required":false,"type":"optional<utf8_string>"}],"coverage":{"execution_domain":"frontend.clang22.type-observation.compile-unit"},"descriptor_id":"frontend.clang22.type_observation.v2","evolution_policy":"ng0.additive.v1","generated_cpp_tag":null,"indexes":[["frontend.clang22.type_observation.v2.semantic_key"]],"merge":{"conflict_columns":["frontend.clang22.type_observation.v2.compile_unit","frontend.clang22.type_observation.v2.exact_equivalence","frontend.clang22.type_observation.v2.limitation","frontend.clang22.type_observation.v2.payload_digest","frontend.clang22.type_observation.v2.semantic_key"],"mode":"functional_assertion"},"name":"frontend.clang22.type_observation","owner_namespace":"cxxlens.clang22.reference","partition":{"condition_fragment":"envelope","interpretation_domain":"envelope","suggested_keys":["frontend.clang22.type_observation.v2.compile_unit"]},"profile":"NG0","provenance":{"minimum":"direct_observation"},"references":[{"on_missing":"reject_batch","source_columns":["frontend.clang22.type_observation.v2.compile_unit"],"strength":"hard","target_columns":["build.compile_unit.v1.compile_unit"],"target_relation":"build.compile_unit"}],"semantic_major":2,"semantics":"frontend.clang22.type_observation/2","stability":"versioned","summary":"Provider-owned Clang 22 structural type observation without source authority.","version":"2.0.0"})cxxlens";
			return value;
		}

		[[nodiscard]] const std::string& call_contract_canonical()
		{
			static const std::string value = []
			{
				auto output = entity_contract_canonical();
				replace_all(output, "entity_observation", "call_observation");
				replace_all(output, "entity-observation", "call-observation");
				replace_all(output, "entity occurrence", "call occurrence");
				return output;
			}();
			return value;
		}

		[[nodiscard]] std::string_view descriptor_id(const observation_v2_kind kind)
		{
			switch (kind)
			{
				case observation_v2_kind::entity:
					return "frontend.clang22.entity_observation.v2";
				case observation_v2_kind::type:
					return "frontend.clang22.type_observation.v2";
				case observation_v2_kind::call:
					return "frontend.clang22.call_observation.v2";
			}
			return {};
		}

		[[nodiscard]] sdk::relation_descriptor make_descriptor(const observation_v2_kind kind)
		{
			const bool has_source = kind != observation_v2_kind::type;
			const std::string id{descriptor_id(kind)};
			const std::string name = id.substr(0U, id.size() - std::string_view{".v2"}.size());
			const std::string prefix = id + '.';

			sdk::relation_descriptor descriptor;
			descriptor.id = id;
			descriptor.name = name;
			descriptor.version = {2U, 0U, 0U};
			descriptor.semantic_major = 2U;
			descriptor.semantics = name + "/2";
			descriptor.owner_namespace = "cxxlens.clang22.reference";
			descriptor.columns = {
				column(prefix,
					   "observation",
					   {sdk::scalar_kind::typed_id, "clang22_observation_id", false},
					   true,
					   sdk::column_role::claim_key),
				column(prefix,
					   "compile_unit",
					   {sdk::scalar_kind::typed_id, "compile_unit_id", false},
					   true,
					   sdk::column_role::authoritative_payload),
				column(prefix,
					   "semantic_key",
					   {sdk::scalar_kind::bytes, {}, false},
					   true,
					   sdk::column_role::authoritative_payload),
				column(prefix,
					   "payload_digest",
					   {sdk::scalar_kind::digest, {}, false},
					   true,
					   sdk::column_role::authoritative_payload),
			};
			if (has_source)
			{
				descriptor.columns.push_back(
					column(prefix,
						   "source",
						   {sdk::scalar_kind::typed_id, "source_span_id", true},
						   false,
						   sdk::column_role::authoritative_payload));
				descriptor.columns.push_back(
					column(prefix,
						   "source_snapshot",
						   {sdk::scalar_kind::typed_id, "source_snapshot_id", true},
						   false,
						   sdk::column_role::authoritative_payload));
				descriptor.columns.push_back(column(prefix,
													"source_file",
													{sdk::scalar_kind::typed_id, "file_id", true},
													false,
													sdk::column_role::authoritative_payload));
				descriptor.columns.push_back(column(prefix,
													"source_begin",
													{sdk::scalar_kind::unsigned_integer, {}, true},
													false,
													sdk::column_role::authoritative_payload));
				descriptor.columns.push_back(column(prefix,
													"source_end",
													{sdk::scalar_kind::unsigned_integer, {}, true},
													false,
													sdk::column_role::authoritative_payload));
				descriptor.columns.push_back(
					column(prefix,
						   "source_role",
						   {sdk::scalar_kind::open_symbol, "source.range-role/1", true},
						   false,
						   sdk::column_role::authoritative_payload));
				descriptor.columns.push_back(column(prefix,
													"source_read_only",
													{sdk::scalar_kind::boolean, {}, true},
													false,
													sdk::column_role::authoritative_payload));
				descriptor.columns.push_back(column(prefix,
													"source_origin_chain",
													{sdk::scalar_kind::bytes, {}, true},
													false,
													sdk::column_role::authoritative_payload));
			}
			descriptor.columns.push_back(column(prefix,
												"exact_equivalence",
												{sdk::scalar_kind::boolean, {}, false},
												true,
												sdk::column_role::authoritative_payload));
			descriptor.columns.push_back(column(prefix,
												"limitation",
												{sdk::scalar_kind::utf8_string, {}, true},
												false,
												sdk::column_role::authoritative_payload));

			descriptor.key_columns = {prefix + "observation"};
			descriptor.domain_identity.result_column = prefix + "observation";
			descriptor.domain_identity.projection = {
				prefix + "compile_unit", prefix + "semantic_key", prefix + "payload_digest"};
			if (has_source)
			{
				for (const auto suffix : {"source",
										  "source_snapshot",
										  "source_file",
										  "source_begin",
										  "source_end",
										  "source_role",
										  "source_read_only",
										  "source_origin_chain"})
					descriptor.domain_identity.projection.push_back(prefix + suffix);
			}
			descriptor.domain_identity.contract = "canonical-binary-tuple-v1";
			descriptor.references.push_back({{prefix + "compile_unit"},
											 "build.compile_unit",
											 {"build.compile_unit.v1.compile_unit"},
											 sdk::reference_strength::hard});
			if (has_source)
				descriptor.references.push_back({{prefix + "source",
												  prefix + "source_snapshot",
												  prefix + "source_file",
												  prefix + "source_begin",
												  prefix + "source_end",
												  prefix + "source_role",
												  prefix + "source_read_only"},
												 "source.span",
												 {"source.span.v1.span",
												  "source.span.v1.snapshot",
												  "source.span.v1.file",
												  "source.span.v1.begin",
												  "source.span.v1.end",
												  "source.span.v1.role",
												  "source.span.v1.read_only"},
												 sdk::reference_strength::hard});
			descriptor.merge = sdk::merge_mode::functional_assertion;
			for (const auto& value : descriptor.columns)
				if (value.role == sdk::column_role::authoritative_payload)
					descriptor.conflict_columns.push_back(value.id);

			switch (kind)
			{
				case observation_v2_kind::entity:
					descriptor.contract_canonical = entity_contract_canonical();
					descriptor.contract_digest =
						"sha256:4a5012801fcde26110a9f6350177d74d7d6975edde96337d4d3918ca7a004d51";
					descriptor.descriptor_digest =
						"semantic-v2:sha256:"
						"eb909eec97cec22586f4ac67dc7c56cc29390857df9355186feae5e9ce7700fb";
					break;
				case observation_v2_kind::type:
					descriptor.contract_canonical = type_contract_canonical();
					descriptor.contract_digest =
						"sha256:53c54f967eb041e75ea98463c212d259fed0d3a310038ac9c93209749e72387f";
					descriptor.descriptor_digest =
						"semantic-v2:sha256:"
						"94b6f6efcd46dad74c0cec1c761a2d363c6acdfe135862c37d0b7e28b01b6026";
					break;
				case observation_v2_kind::call:
					descriptor.contract_canonical = call_contract_canonical();
					descriptor.contract_digest =
						"sha256:07ea48a7f00e80972ba59c14ee96f916772ad9ed57fc84e313e3958f08fa548a";
					descriptor.descriptor_digest =
						"semantic-v2:sha256:"
						"8b79a9fb3d59e750c51310d6f32935701a36c68fd5830228516482b0e7d2cd65";
					break;
			}
			return descriptor;
		}

		[[nodiscard]] sdk::result<std::string>
		payload_digest(const std::string_view id,
					   const std::vector<observation_v2_payload_entry>& payload)
		{
			std::vector<sdk::canonical_value> entries;
			entries.reserve(payload.size());
			for (std::size_t index{}; index < payload.size(); ++index)
			{
				const auto& entry = payload[index];
				if (entry.key.empty() || !sdk::validate_utf8_text(entry.key) ||
					!sdk::validate_utf8_text(entry.value))
					return sdk::unexpected(observation_error("payload", "strict-utf8"));
				if (index != 0U && !utf8_byte_less(payload[index - 1U].key, entry.key))
					return sdk::unexpected(
						observation_error("payload", "unique-strict-utf8-byte-order"));
				entries.push_back(sdk::canonical_value::from_tuple(
					{sdk::canonical_value::from_string(entry.key),
					 sdk::canonical_value::from_string(entry.value)}));
			}
			auto encoded = sdk::canonical_binary(sdk::canonical_value::from_tuple(
				{sdk::canonical_value::from_string(std::string{payload_domain}),
				 sdk::canonical_value::from_string(std::string{id}),
				 sdk::canonical_value::from_tuple(std::move(entries))}));
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return sdk::semantic_digest(
				payload_domain,
				std::string_view{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		}

		[[nodiscard]] sdk::result<std::optional<std::vector<std::byte>>>
		origin_bytes(const std::vector<observation_v2_origin>& origins)
		{
			if (origins.empty())
				return std::optional<std::vector<std::byte>>{};
			std::vector<sdk::canonical_value> entries;
			entries.reserve(origins.size());
			for (const auto& origin : origins)
			{
				if (origin.kind.empty() || origin.logical_path.empty() ||
					!sdk::validate_utf8_text(origin.kind) ||
					!sdk::validate_utf8_text(origin.logical_path) || origin.begin < 0 ||
					origin.end < origin.begin || !origin.read_only)
					return sdk::unexpected(observation_error("origin_chain", "native-v2-domain"));
				entries.push_back(sdk::canonical_value::from_tuple(
					{sdk::canonical_value::from_string(origin.kind),
					 sdk::canonical_value::from_string(origin.logical_path),
					 sdk::canonical_value::from_integer(origin.begin),
					 sdk::canonical_value::from_integer(origin.end),
					 sdk::canonical_value::from_boolean(true)}));
			}
			auto encoded = sdk::canonical_binary(sdk::canonical_value::from_tuple(
				{sdk::canonical_value::from_string(std::string{origin_projection_tag}),
				 sdk::canonical_value::from_tuple(std::move(entries))}));
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return std::optional<std::vector<std::byte>>{std::move(*encoded)};
		}

		[[nodiscard]] sdk::result<void> validate_span(const observation_v2_primary_span& span,
													  const observation_v2_task_authority& task)
		{
			for (const auto& [field, value] : {
					 std::pair{std::string_view{"span_id"}, std::string_view{span.span_id}},
					 std::pair{std::string_view{"snapshot"}, std::string_view{span.snapshot}},
					 std::pair{std::string_view{"file"}, std::string_view{span.file}},
					 std::pair{std::string_view{"role"}, std::string_view{span.role}},
				 })
				if (!sdk::validate_strong_id(value))
					return sdk::unexpected(span_error(std::string{field}, "strong-id"));
			if (span.begin > span.end || span.end > task.source_size_bytes)
				return sdk::unexpected(span_error("range", "outside-task-source"));
			if (span.snapshot != task.source_snapshot_id || span.file != task.source_file_id)
				return sdk::unexpected(span_error("source", "task-binding"));
			auto expected = sdk::source_span_identity(
				span.snapshot, span.file, span.begin, span.end, span.role);
			if (!expected)
				return sdk::unexpected(span_error("range", "signed-int64-domain"));
			if (*expected != span.span_id)
				return sdk::unexpected(span_error("span_id", "identity-mismatch"));
			return {};
		}

		[[nodiscard]] sdk::result<observation_v2_kind>
		kind_from_descriptor_id(const std::string_view id)
		{
			for (const auto kind : {observation_v2_kind::entity,
									observation_v2_kind::type,
									observation_v2_kind::call})
				if (id == descriptor_id(kind))
					return kind;
			return sdk::unexpected(sdk::error{
				"materialization.descriptor-binding-mismatch", "descriptor_id", std::string{id}});
		}

		[[nodiscard]] bool semantic_digest_spelling(const std::string_view value) noexcept
		{
			constexpr std::string_view prefix{"semantic-v2:sha256:"};
			const auto hex =
				value.starts_with(prefix) ? value.substr(prefix.size()) : std::string_view{};
			return hex.size() == 64U &&
				std::ranges::all_of(hex,
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] sdk::result<const sdk::detached_cell*> present_cell(
			const sdk::detached_row& row, const std::string_view suffix, const bool required)
		{
			const auto id = row.descriptor_id + '.' + std::string{suffix};
			const auto found = row.cells.find(id);
			if (found == row.cells.end() || found->second.state == sdk::cell_state::absent)
			{
				if (!required)
					return nullptr;
				return sdk::unexpected(observation_error(std::string{suffix}, "required-present"));
			}
			if (found->second.state != sdk::cell_state::present || !found->second.value)
				return sdk::unexpected(observation_error(std::string{suffix}, "unresolved-cell"));
			return &found->second;
		}

		template <class Value>
		[[nodiscard]] sdk::result<Value> required_value(const sdk::detached_row& row,
														const std::string_view suffix)
		{
			auto found = present_cell(row, suffix, true);
			if (!found)
				return sdk::unexpected(std::move(found.error()));
			const auto* value = std::get_if<Value>(&*(*found)->value);
			if (value == nullptr)
				return sdk::unexpected(observation_error(std::string{suffix}, "scalar-type"));
			return *value;
		}

		template <class Value>
		[[nodiscard]] sdk::result<std::optional<Value>>
		optional_value(const sdk::detached_row& row, const std::string_view suffix)
		{
			auto found = present_cell(row, suffix, false);
			if (!found)
				return sdk::unexpected(std::move(found.error()));
			if (*found == nullptr)
				return std::optional<Value>{};
			const auto* value = std::get_if<Value>(&*(*found)->value);
			if (value == nullptr)
				return sdk::unexpected(observation_error(std::string{suffix}, "scalar-type"));
			return std::optional<Value>{*value};
		}

		[[nodiscard]] sdk::result<std::vector<observation_v2_origin>>
		decode_origin_bytes(const std::span<const std::byte> bytes)
		{
			auto decoded = sdk::canonical_binary_decode(bytes);
			if (!decoded || decoded->type != sdk::canonical_value::kind::ordered_tuple ||
				decoded->tuple.size() != 2U ||
				decoded->tuple[0U].type != sdk::canonical_value::kind::utf8_string ||
				decoded->tuple[0U].text != origin_projection_tag ||
				decoded->tuple[1U].type != sdk::canonical_value::kind::ordered_tuple ||
				decoded->tuple[1U].tuple.empty())
				return sdk::unexpected(observation_error("origin_chain", "canonical-shape"));

			std::vector<observation_v2_origin> output;
			output.reserve(decoded->tuple[1U].tuple.size());
			for (const auto& value : decoded->tuple[1U].tuple)
			{
				if (value.type != sdk::canonical_value::kind::ordered_tuple ||
					value.tuple.size() != 5U ||
					value.tuple[0U].type != sdk::canonical_value::kind::utf8_string ||
					value.tuple[1U].type != sdk::canonical_value::kind::utf8_string ||
					value.tuple[2U].type != sdk::canonical_value::kind::signed_integer ||
					value.tuple[3U].type != sdk::canonical_value::kind::signed_integer ||
					value.tuple[4U].type != sdk::canonical_value::kind::boolean)
					return sdk::unexpected(observation_error("origin_chain", "entry-shape"));
				observation_v2_origin origin{value.tuple[0U].text,
											 value.tuple[1U].text,
											 value.tuple[2U].integer,
											 value.tuple[3U].integer,
											 value.tuple[4U].boolean};
				if (origin.kind.empty() || origin.logical_path.empty() || origin.begin < 0 ||
					origin.end < origin.begin || !origin.read_only)
					return sdk::unexpected(observation_error("origin_chain", "native-v2-domain"));
				output.push_back(std::move(origin));
			}

			auto reencoded = origin_bytes(output);
			if (!reencoded || !reencoded->has_value() || !std::ranges::equal(**reencoded, bytes))
				return sdk::unexpected(observation_error("origin_chain", "projection-mismatch"));
			return output;
		}
	} // namespace

	const sdk::relation_descriptor& entity_observation_v2_descriptor()
	{
		static const sdk::relation_descriptor descriptor =
			make_descriptor(observation_v2_kind::entity);
		return descriptor;
	}

	const sdk::relation_descriptor& type_observation_v2_descriptor()
	{
		static const sdk::relation_descriptor descriptor =
			make_descriptor(observation_v2_kind::type);
		return descriptor;
	}

	const sdk::relation_descriptor& call_observation_v2_descriptor()
	{
		static const sdk::relation_descriptor descriptor =
			make_descriptor(observation_v2_kind::call);
		return descriptor;
	}

	sdk::result<const sdk::relation_descriptor*>
	observation_v2_descriptor(const observation_v2_kind kind)
	{
		switch (kind)
		{
			case observation_v2_kind::entity:
				return &entity_observation_v2_descriptor();
			case observation_v2_kind::type:
				return &type_observation_v2_descriptor();
			case observation_v2_kind::call:
				return &call_observation_v2_descriptor();
		}
		return sdk::unexpected(observation_error("kind", "unsupported"));
	}

	sdk::result<sdk::detached_row>
	make_observation_v2_row(const native_observation_v2& record,
							const observation_v2_task_authority& task)
	{
		auto descriptor_result = observation_v2_descriptor(record.kind);
		if (!descriptor_result)
			return sdk::unexpected(std::move(descriptor_result.error()));
		const auto& descriptor = **descriptor_result;
		if (auto valid = descriptor.validate(); !valid)
			return sdk::unexpected(sdk::error{
				"materialization.descriptor-binding-mismatch", "descriptor", valid.error().code});
		if (!sdk::validate_strong_id(task.final_relation_compile_unit_id) ||
			!sdk::validate_strong_id(record.final_relation_compile_unit_id) ||
			record.final_relation_compile_unit_id != task.final_relation_compile_unit_id)
			return sdk::unexpected(
				observation_error("final_relation_compile_unit_id", "task-binding"));
		if (record.semantic_key.empty() || !sdk::validate_utf8_text(record.semantic_key))
			return sdk::unexpected(observation_error("semantic_key", "nonempty-strict-utf8"));
		if (record.limitation &&
			(record.limitation->empty() || !sdk::validate_utf8_text(*record.limitation)))
			return sdk::unexpected(observation_error("limitation", "nonempty-strict-utf8"));
		if (record.exact_equivalence != !record.limitation.has_value())
			return sdk::unexpected(observation_error("exact_equivalence", "limitation-coupling"));

		auto encoded_origins = origin_bytes(record.origin_chain);
		if (!encoded_origins)
			return sdk::unexpected(std::move(encoded_origins.error()));
		if (record.kind == observation_v2_kind::type &&
			(record.primary_span.has_value() || encoded_origins->has_value()))
			return sdk::unexpected(observation_error("type", "source-authority-forbidden"));
		if (record.primary_span)
			if (auto valid = validate_span(*record.primary_span, task); !valid)
				return sdk::unexpected(std::move(valid.error()));

		auto digest = payload_digest(descriptor.id, record.payload);
		if (!digest)
			return sdk::unexpected(std::move(digest.error()));

		const std::string prefix = descriptor.id + '.';
		sdk::detached_row row{descriptor.id, {}};
		const auto add = [&](const std::string_view name, sdk::detached_cell value)
		{
			row.cells.emplace(prefix + std::string{name}, std::move(value));
		};
		add("compile_unit",
			cell({sdk::scalar_kind::typed_id, "compile_unit_id", false},
				 sdk::scalar_value{record.final_relation_compile_unit_id}));
		add("semantic_key",
			cell({sdk::scalar_kind::bytes, {}, false},
				 sdk::scalar_value{exact_bytes(record.semantic_key)}));
		add("payload_digest",
			cell({sdk::scalar_kind::digest, {}, false}, sdk::scalar_value{std::move(*digest)}));
		if (record.primary_span)
		{
			const auto& span = *record.primary_span;
			add("source",
				cell({sdk::scalar_kind::typed_id, "source_span_id", true},
					 sdk::scalar_value{span.span_id}));
			add("source_snapshot",
				cell({sdk::scalar_kind::typed_id, "source_snapshot_id", true},
					 sdk::scalar_value{span.snapshot}));
			add("source_file",
				cell({sdk::scalar_kind::typed_id, "file_id", true}, sdk::scalar_value{span.file}));
			add("source_begin",
				cell({sdk::scalar_kind::unsigned_integer, {}, true},
					 sdk::scalar_value{span.begin}));
			add("source_end",
				cell({sdk::scalar_kind::unsigned_integer, {}, true}, sdk::scalar_value{span.end}));
			add("source_role",
				cell({sdk::scalar_kind::open_symbol, "source.range-role/1", true},
					 sdk::scalar_value{span.role}));
			add("source_read_only",
				cell({sdk::scalar_kind::boolean, {}, true}, sdk::scalar_value{span.read_only}));
		}
		if (encoded_origins->has_value())
			add("source_origin_chain",
				cell({sdk::scalar_kind::bytes, {}, true},
					 sdk::scalar_value{std::move(**encoded_origins)}));
		add("exact_equivalence",
			cell({sdk::scalar_kind::boolean, {}, false},
				 sdk::scalar_value{record.exact_equivalence}));
		if (record.limitation)
			add("limitation",
				cell({sdk::scalar_kind::utf8_string, {}, true},
					 sdk::scalar_value{*record.limitation}));

		auto identity = sdk::derive_domain_identity(descriptor, row);
		if (!identity)
			return sdk::unexpected(std::move(identity.error()));
		add("observation",
			cell({sdk::scalar_kind::typed_id, "clang22_observation_id", false},
				 sdk::scalar_value{std::move(*identity)}));
		if (auto valid = sdk::validate_row(descriptor, row); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid = sdk::validate_domain_identity(descriptor, row); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return row;
	}

	sdk::result<decoded_observation_v2_row>
	decode_observation_v2_row(const sdk::detached_row& row,
							  const observation_v2_task_authority& task)
	{
		auto kind = kind_from_descriptor_id(row.descriptor_id);
		if (!kind)
			return sdk::unexpected(std::move(kind.error()));
		auto descriptor_result = observation_v2_descriptor(*kind);
		if (!descriptor_result)
			return sdk::unexpected(std::move(descriptor_result.error()));
		const auto& descriptor = **descriptor_result;
		if (auto valid = descriptor.validate(); !valid)
			return sdk::unexpected(sdk::error{
				"materialization.descriptor-binding-mismatch", "descriptor", valid.error().code});
		if (auto valid = sdk::validate_row(descriptor, row); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid = sdk::validate_domain_identity(descriptor, row); !valid)
			return sdk::unexpected(std::move(valid.error()));

		if (!sdk::validate_strong_id(task.final_relation_compile_unit_id))
			return sdk::unexpected(
				observation_error("final_relation_compile_unit_id", "task-strong-id"));
		auto compile_unit = required_value<std::string>(row, "compile_unit");
		auto semantic_key_bytes = required_value<std::vector<std::byte>>(row, "semantic_key");
		auto digest = required_value<std::string>(row, "payload_digest");
		auto exact = required_value<bool>(row, "exact_equivalence");
		auto limitation = optional_value<std::string>(row, "limitation");
		if (!compile_unit || !semantic_key_bytes || !digest || !exact || !limitation)
		{
			if (!compile_unit)
				return sdk::unexpected(std::move(compile_unit.error()));
			if (!semantic_key_bytes)
				return sdk::unexpected(std::move(semantic_key_bytes.error()));
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			if (!exact)
				return sdk::unexpected(std::move(exact.error()));
			return sdk::unexpected(std::move(limitation.error()));
		}
		if (!sdk::validate_strong_id(*compile_unit) ||
			*compile_unit != task.final_relation_compile_unit_id)
			return sdk::unexpected(
				observation_error("final_relation_compile_unit_id", "task-binding"));
		std::string semantic_key;
		if (!semantic_key_bytes->empty())
			semantic_key.assign(reinterpret_cast<const char*>(semantic_key_bytes->data()),
								semantic_key_bytes->size());
		if (semantic_key.empty() || !sdk::validate_utf8_text(semantic_key))
			return sdk::unexpected(observation_error("semantic_key", "nonempty-strict-utf8"));
		if (!semantic_digest_spelling(*digest))
			return sdk::unexpected(
				observation_error("payload_digest", "semantic-v2-descriptor-domain"));
		if (limitation->has_value() &&
			((*limitation)->empty() || !sdk::validate_utf8_text(**limitation)))
			return sdk::unexpected(observation_error("limitation", "nonempty-strict-utf8"));
		if (*exact != !limitation->has_value())
			return sdk::unexpected(observation_error("exact_equivalence", "limitation-coupling"));

		std::optional<observation_v2_primary_span> primary_span;
		std::vector<observation_v2_origin> origins;
		if (*kind != observation_v2_kind::type)
		{
			auto source = optional_value<std::string>(row, "source");
			auto snapshot = optional_value<std::string>(row, "source_snapshot");
			auto file = optional_value<std::string>(row, "source_file");
			auto begin = optional_value<std::uint64_t>(row, "source_begin");
			auto end = optional_value<std::uint64_t>(row, "source_end");
			auto role = optional_value<std::string>(row, "source_role");
			auto read_only = optional_value<bool>(row, "source_read_only");
			if (!source || !snapshot || !file || !begin || !end || !role || !read_only)
			{
				if (!source)
					return sdk::unexpected(std::move(source.error()));
				if (!snapshot)
					return sdk::unexpected(std::move(snapshot.error()));
				if (!file)
					return sdk::unexpected(std::move(file.error()));
				if (!begin)
					return sdk::unexpected(std::move(begin.error()));
				if (!end)
					return sdk::unexpected(std::move(end.error()));
				if (!role)
					return sdk::unexpected(std::move(role.error()));
				return sdk::unexpected(std::move(read_only.error()));
			}
			const auto present_count = static_cast<unsigned>(source->has_value()) +
				static_cast<unsigned>(snapshot->has_value()) +
				static_cast<unsigned>(file->has_value()) +
				static_cast<unsigned>(begin->has_value()) +
				static_cast<unsigned>(end->has_value()) + static_cast<unsigned>(role->has_value()) +
				static_cast<unsigned>(read_only->has_value());
			if (present_count != 0U && present_count != 7U)
				return sdk::unexpected(observation_error("primary_span", "all-or-none"));
			if (present_count == 7U)
			{
				primary_span = observation_v2_primary_span{
					**source, **snapshot, **file, **begin, **end, **role, **read_only};
				if (auto valid = validate_span(*primary_span, task); !valid)
					return sdk::unexpected(std::move(valid.error()));
			}

			auto origin_bytes_value =
				optional_value<std::vector<std::byte>>(row, "source_origin_chain");
			if (!origin_bytes_value)
				return sdk::unexpected(std::move(origin_bytes_value.error()));
			if (origin_bytes_value->has_value())
			{
				auto decoded = decode_origin_bytes(**origin_bytes_value);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				origins = std::move(*decoded);
			}
		}

		return decoded_observation_v2_row{*kind,
										  std::move(*compile_unit),
										  std::move(semantic_key),
										  std::move(*digest),
										  std::move(primary_span),
										  std::move(origins),
										  *exact,
										  std::move(*limitation)};
	}
} // namespace cxxlens::detail::clang22::materialization
