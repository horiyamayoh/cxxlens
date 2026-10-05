#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_body.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_cfg_edge.hpp>
#include <cxxlens/relations/cc_cfg_node.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_entity_detail.hpp>
#include <cxxlens/relations/cc_entity_edge.hpp>
#include <cxxlens/relations/cc_flow_fact.hpp>
#include <cxxlens/relations/cc_layout_fact.hpp>
#include <cxxlens/relations/cc_record_inventory.hpp>
#include <cxxlens/relations/cc_record_surface.hpp>
#include <cxxlens/relations/cc_syntax_node.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/cc_type_component.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/relations/core_claim_conflict.hpp>
#include <cxxlens/relations/core_differential_disagreement.hpp>
#include <cxxlens/relations/core_provider_execution.hpp>
#include <cxxlens/relations/core_unresolved.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_include.hpp>
#include <cxxlens/relations/source_origin.hpp>
#include <cxxlens/relations/source_preprocessor_event.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/relations/source_token.hpp>
#include <cxxlens/relations/source_token_inventory.hpp>
#include <cxxlens/sdk/relation.hpp>

namespace cxxlens::sdk
{
	namespace
	{
		enum class observation_relation_kind
		{
			entity,
			type,
			call
		};

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

		[[nodiscard]] std::string_view descriptor_id(const observation_relation_kind kind)
		{
			switch (kind)
			{
				case observation_relation_kind::entity:
					return "frontend.clang22.entity_observation.v2";
				case observation_relation_kind::type:
					return "frontend.clang22.type_observation.v2";
				case observation_relation_kind::call:
					return "frontend.clang22.call_observation.v2";
			}
			return {};
		}

		[[nodiscard]] sdk::relation_descriptor make_descriptor(const observation_relation_kind kind)
		{
			const bool has_source = kind != observation_relation_kind::type;
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
				case observation_relation_kind::entity:
					descriptor.contract_canonical = entity_contract_canonical();
					descriptor.contract_digest =
						"sha256:4a5012801fcde26110a9f6350177d74d7d6975edde96337d4d3918ca7a004d51";
					descriptor.descriptor_digest =
						"semantic-v2:sha256:"
						"eb909eec97cec22586f4ac67dc7c56cc29390857df9355186feae5e9ce7700fb";
					break;
				case observation_relation_kind::type:
					descriptor.contract_canonical = type_contract_canonical();
					descriptor.contract_digest =
						"sha256:53c54f967eb041e75ea98463c212d259fed0d3a310038ac9c93209749e72387f";
					descriptor.descriptor_digest =
						"semantic-v2:sha256:"
						"94b6f6efcd46dad74c0cec1c761a2d363c6acdfe135862c37d0b7e28b01b6026";
					break;
				case observation_relation_kind::call:
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

	} // namespace

	std::span<const relation_descriptor> standard_relation_descriptors()
	{
		static const std::vector<relation_descriptor> values = []
		{
			std::vector<relation_descriptor> descriptors{
				build::relations::compile_unit::descriptor(),
				build::relations::project::descriptor(),
				build::relations::toolchain_context::descriptor(),
				build::relations::variant::descriptor(),
				cc::relations::body::descriptor(),
				cc::relations::call_direct_target::descriptor(),
				cc::relations::call_site::descriptor(),
				cc::relations::cfg_edge::descriptor(),
				cc::relations::cfg_node::descriptor(),
				cc::relations::declaration::descriptor(),
				cc::relations::entity::descriptor(),
				cc::relations::entity_detail::descriptor(),
				cc::relations::entity_edge::descriptor(),
				cc::relations::flow_fact::descriptor(),
				cc::relations::layout_fact::descriptor(),
				cc::relations::record_surface::descriptor(),
				cc::relations::record_inventory::descriptor(),
				cc::relations::syntax_node::descriptor(),
				cc::relations::type::descriptor(),
				cc::relations::type_component::descriptor(),
				company::relations::lock_acquire::descriptor(),
				core::relations::claim_conflict::descriptor(),
				core::relations::differential_disagreement::descriptor(),
				core::relations::provider_execution::descriptor(),
				core::relations::unresolved::descriptor(),
				source::relations::file::descriptor(),
				source::relations::include::descriptor(),
				source::relations::origin::descriptor(),
				source::relations::preprocessor_event::descriptor(),
				source::relations::span::descriptor(),
				source::relations::token::descriptor(),
				source::relations::token_inventory::descriptor(),
				make_descriptor(observation_relation_kind::entity),
				make_descriptor(observation_relation_kind::type),
				make_descriptor(observation_relation_kind::call)};
			std::ranges::sort(descriptors, {}, &relation_descriptor::id);
			return descriptors;
		}();
		return values;
	}
} // namespace cxxlens::sdk
