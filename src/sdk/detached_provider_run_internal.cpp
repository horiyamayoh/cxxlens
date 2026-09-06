#include "detached_provider_run_internal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <ranges>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "bounded_canonical_binary_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::size_t maximum_protocol_bytes{std::size_t{16U} * 1024U * 1024U};
		constexpr std::size_t maximum_partitions{4096U};
		constexpr std::size_t maximum_diagnostics{10000U};
		constexpr std::uint64_t maximum_rows{100000U};
		constexpr std::uint64_t maximum_canonical_values{200000U};

		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.detached-provider-run-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] error limit(std::string field, std::string detail)
		{
			return {
				"application-analysis.import-limit-exceeded", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool digest_like(const std::string_view value)
		{
			const auto marker = value.rfind("sha256:");
			return marker != std::string_view::npos && marker + 7U + 64U == value.size() &&
				std::ranges::all_of(value.substr(marker + 7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] result<void> text(const std::string_view value,
										const std::string& field,
										const bool allow_empty,
										const import_limits& limits,
										std::size_t& metadata_bytes)
		{
			if ((!allow_empty && value.empty()) || value.size() > limits.maximum_string_bytes)
				return unexpected(limit(field, "string-bytes"));
			if (value.size() > limits.maximum_total_metadata_bytes - metadata_bytes)
				return unexpected(limit("metadata", "total-bytes"));
			if (auto valid = validate_utf8_text(value); !valid)
				return unexpected(invalid(field, "utf8"));
			metadata_bytes += value.size();
			return {};
		}

		[[nodiscard]] result<void> strong_id(const std::string_view value, const std::string& field)
		{
			if (auto valid = validate_strong_id(value); !valid)
				return unexpected(invalid(field, "strong-id"));
			return {};
		}

		[[nodiscard]] std::string_view terminal_name(const detached_provider_terminal value)
		{
			switch (value)
			{
				case detached_provider_terminal::complete:
					return "complete";
				case detached_provider_terminal::partial:
					return "partial";
				case detached_provider_terminal::rejected:
					return "rejected";
				case detached_provider_terminal::failed:
					return "failed";
				case detached_provider_terminal::cancelled:
					return "cancelled";
			}
			return {};
		}

		[[nodiscard]] result<detached_provider_terminal> terminal_from(const std::string_view value)
		{
			for (const auto terminal : {detached_provider_terminal::complete,
										detached_provider_terminal::partial,
										detached_provider_terminal::rejected,
										detached_provider_terminal::failed,
										detached_provider_terminal::cancelled})
				if (value == terminal_name(terminal))
					return terminal;
			return unexpected(invalid("terminal", "closed-enum"));
		}

		[[nodiscard]] canonical_value version_value(const semantic_version& value)
		{
			return canonical_value::from_tuple({
				canonical_value::from_integer(value.major),
				canonical_value::from_integer(value.minor),
				canonical_value::from_integer(value.patch),
			});
		}

		[[nodiscard]] canonical_value identity_value(const detached_provider_identity& value)
		{
			return canonical_value::from_tuple({
				canonical_value::from_string(value.provider_id),
				version_value(value.provider_version),
				canonical_value::from_string(value.binary_digest),
				canonical_value::from_string(value.semantic_contract_digest),
				canonical_value::from_string(value.signature_digest),
				canonical_value::from_string(value.revocation_state),
				canonical_value::from_string(value.sandbox_policy_digest),
			});
		}

		template <class value_type, class projection>
		[[nodiscard]] std::vector<canonical_value>
		collection_values(const std::vector<value_type>& values, projection project)
		{
			std::vector<canonical_value> output;
			output.reserve(values.size());
			for (const auto& value : values)
				output.push_back(project(value));
			return output;
		}

		[[nodiscard]] canonical_value partition_value(const detached_partition_projection& value)
		{
			return canonical_value::from_tuple({
				canonical_value::from_string(value.descriptor_id),
				canonical_value::from_string(value.descriptor_digest),
				canonical_value::from_string(value.dependency_group_id),
				canonical_value::from_string(value.atomic_output_group_id),
				canonical_value::from_string(value.batch_id),
				canonical_value::from_string(value.batch_digest),
				canonical_value::from_integer(static_cast<std::int64_t>(value.row_count)),
			});
		}

		[[nodiscard]] canonical_value coverage_value(const detached_coverage_projection& value)
		{
			return canonical_value::from_tuple({
				canonical_value::from_string(value.kind),
				canonical_value::from_string(value.id),
				canonical_value::from_string(value.state),
				canonical_value::from_string(value.reason),
			});
		}

		[[nodiscard]] canonical_value unresolved_value(const detached_unresolved_projection& value)
		{
			return canonical_value::from_tuple({
				canonical_value::from_string(value.code),
				canonical_value::from_string(value.subject),
				canonical_value::from_string(value.detail),
			});
		}

		[[nodiscard]] canonical_value provenance_value(const detached_provenance_projection& value)
		{
			return canonical_value::from_tuple({
				canonical_value::from_string(value.kind),
				canonical_value::from_string(value.subject),
				canonical_value::from_string(value.producer),
				canonical_value::from_string(value.summary),
			});
		}

		[[nodiscard]] canonical_value
		authentication_value(const detached_provider_run_authentication& value)
		{
			return canonical_value::from_tuple({
				canonical_value::from_string(value.algorithm),
				canonical_value::from_string(value.signer_id),
				canonical_value::from_string(value.key_fingerprint),
				canonical_value::from_string(value.signed_subject_digest),
				canonical_value::from_bytes({value.signature.begin(), value.signature.end()}),
				canonical_value::from_string(value.signature_digest),
			});
		}

		[[nodiscard]] auto partition_order(const detached_partition_projection& value)
		{
			return std::tie(value.descriptor_id,
							value.dependency_group_id,
							value.atomic_output_group_id,
							value.batch_id);
		}

		[[nodiscard]] auto coverage_order(const detached_coverage_projection& value)
		{
			return std::tie(value.kind, value.id);
		}

		[[nodiscard]] auto unresolved_order(const detached_unresolved_projection& value)
		{
			return std::tie(value.code, value.subject, value.detail);
		}

		[[nodiscard]] auto provenance_order(const detached_provenance_projection& value)
		{
			return std::tie(value.kind, value.subject, value.producer, value.summary);
		}

		void canonicalize_projections(detached_provider_run_draft& draft)
		{
			std::ranges::sort(draft.partitions, {}, partition_order);
			std::ranges::sort(draft.coverage, {}, coverage_order);
			std::ranges::sort(draft.unresolved, {}, unresolved_order);
			std::ranges::sort(draft.provenance, {}, provenance_order);
		}

		[[nodiscard]] std::vector<canonical_value>
		signed_subject_fields(const detached_provider_run_draft& draft)
		{
			return {
				canonical_value::from_string("cxxlens.detached-provider-run.v1"),
				canonical_value::from_string(draft.task_id),
				canonical_value::from_string(draft.task_input_digest),
				canonical_value::from_string(draft.replay_plan_digest),
				identity_value(draft.provider),
				canonical_value::from_bytes(draft.protocol_transcript),
				canonical_value::from_string(std::string{terminal_name(draft.terminal)}),
				canonical_value::from_tuple(collection_values(draft.partitions, partition_value)),
				canonical_value::from_tuple(collection_values(draft.coverage, coverage_value)),
				canonical_value::from_tuple(collection_values(draft.unresolved, unresolved_value)),
				canonical_value::from_tuple(collection_values(draft.provenance, provenance_value)),
				draft.runtime_receipt_digest
					? canonical_value::from_string(*draft.runtime_receipt_digest)
					: canonical_value::null(),
			};
		}

		[[nodiscard]] result<std::vector<std::byte>>
		encode(const detached_provider_run_draft& draft)
		{
			auto fields = signed_subject_fields(draft);
			fields.push_back(authentication_value(draft.authentication));
			return canonical_binary(canonical_value::from_tuple(std::move(fields)));
		}

		[[nodiscard]] result<const std::vector<canonical_value>*>
		tuple(const canonical_value& value, const std::string& field, const std::size_t count)
		{
			if (value.type != canonical_value::kind::ordered_tuple || value.tuple.size() != count)
				return unexpected(invalid(field, "tuple-shape"));
			return &value.tuple;
		}

		[[nodiscard]] result<std::string> string(const canonical_value& value,
												 const std::string& field)
		{
			if (value.type != canonical_value::kind::utf8_string)
				return unexpected(invalid(field, "string"));
			return value.text;
		}

		[[nodiscard]] result<std::uint64_t> unsigned_integer(const canonical_value& value,
															 const std::string& field)
		{
			if (value.type != canonical_value::kind::signed_integer || value.integer < 0)
				return unexpected(invalid(field, "unsigned-integer"));
			return static_cast<std::uint64_t>(value.integer);
		}

		[[nodiscard]] result<semantic_version> version(const canonical_value& value)
		{
			auto fields = tuple(value, "provider_identity.version", 3U);
			if (!fields)
				return unexpected(std::move(fields.error()));
			std::array<std::uint32_t, 3U> components{};
			auto field = (*fields)->begin();
			for (auto& component_value : components)
			{
				auto component = unsigned_integer(*field, "provider_identity.version");
				if (!component || *component > std::numeric_limits<std::uint32_t>::max())
					return unexpected(invalid("provider_identity.version", "component"));
				component_value = static_cast<std::uint32_t>(*component);
				++field;
			}
			return semantic_version{components[0], components[1], components[2]};
		}

		template <class value_type, class decoder_type>
		[[nodiscard]] result<std::vector<value_type>>
		decode_collection(const canonical_value& value,
						  const std::string& field,
						  const std::size_t maximum,
						  decoder_type decode_item)
		{
			if (value.type != canonical_value::kind::ordered_tuple)
				return unexpected(invalid(field, "tuple"));
			if (value.tuple.size() > maximum)
				return unexpected(limit(field, "count"));
			std::vector<value_type> output;
			output.reserve(value.tuple.size());
			for (std::size_t index{}; index < value.tuple.size(); ++index)
			{
				auto decoded =
					decode_item(value.tuple[index], field + "[" + std::to_string(index) + "]");
				if (!decoded)
					return unexpected(std::move(decoded.error()));
				output.push_back(std::move(*decoded));
			}
			return output;
		}
	} // namespace

	result<std::string>
	detached_provider_run_signed_subject_digest(const detached_provider_run_draft& draft)
	{
		try
		{
			auto canonical = draft;
			canonicalize_projections(canonical);
			auto encoded =
				canonical_binary(canonical_value::from_tuple(signed_subject_fields(canonical)));
			if (!encoded)
				return unexpected(invalid("run_authentication", "subject-encoding"));
			return content_digest(*encoded);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("run_authentication", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("run_authentication", "allocation-length"));
		}
	}

	result<validated_detached_provider_run>
	validate_detached_provider_run(detached_provider_run_draft draft, const import_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return unexpected(std::move(valid.error()));
			if (!is_valid(draft.terminal))
				return unexpected(invalid("terminal", "closed-enum"));
			if (draft.protocol_transcript.empty() ||
				draft.protocol_transcript.size() > maximum_protocol_bytes)
				return unexpected(limit("protocol_transcript", "bytes"));
			if (draft.partitions.size() > maximum_partitions ||
				draft.coverage.size() > maximum_diagnostics ||
				draft.unresolved.size() > maximum_diagnostics ||
				draft.provenance.size() > maximum_diagnostics)
				return unexpected(limit("projection", "count"));

			std::size_t metadata_bytes{};
			for (const auto& [field, value] : {
					 std::pair{std::string{"task_id"}, &draft.task_id},
					 std::pair{std::string{"task_input_digest"}, &draft.task_input_digest},
					 std::pair{std::string{"replay_plan_digest"}, &draft.replay_plan_digest},
					 std::pair{std::string{"provider_id"}, &draft.provider.provider_id},
					 std::pair{std::string{"provider_binary_digest"},
							   &draft.provider.binary_digest},
					 std::pair{std::string{"provider_semantic_contract_digest"},
							   &draft.provider.semantic_contract_digest},
					 std::pair{std::string{"provider_signature_digest"},
							   &draft.provider.signature_digest},
					 std::pair{std::string{"provider_revocation_state"},
							   &draft.provider.revocation_state},
					 std::pair{std::string{"sandbox_policy_digest"},
							   &draft.provider.sandbox_policy_digest},
				 })
				if (auto valid = text(*value, field, false, limits, metadata_bytes); !valid)
					return unexpected(std::move(valid.error()));
			if (auto valid = strong_id(draft.task_id, "task_id"); !valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = strong_id(draft.provider.provider_id, "provider_id"); !valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = strong_id(draft.provider.revocation_state, "revocation_state"); !valid)
				return unexpected(std::move(valid.error()));
			for (const auto* value : {&draft.task_input_digest,
									  &draft.replay_plan_digest,
									  &draft.provider.binary_digest,
									  &draft.provider.semantic_contract_digest,
									  &draft.provider.signature_digest,
									  &draft.provider.sandbox_policy_digest})
				if (!digest_like(*value))
					return unexpected(invalid("digest", "spelling"));
			if (draft.runtime_receipt_digest)
			{
				if (auto valid = text(*draft.runtime_receipt_digest,
									  "runtime_receipt_digest",
									  false,
									  limits,
									  metadata_bytes);
					!valid)
					return unexpected(std::move(valid.error()));
				if (!digest_like(*draft.runtime_receipt_digest))
					return unexpected(invalid("runtime_receipt_digest", "spelling"));
			}
			std::uint64_t rows{};
			for (std::size_t index{}; index < draft.partitions.size(); ++index)
			{
				auto& value = draft.partitions[index];
				const auto prefix = "partitions[" + std::to_string(index) + "]";
				for (const auto& [field, member] : {
						 std::pair{prefix + ".descriptor_id", &value.descriptor_id},
						 std::pair{prefix + ".descriptor_digest", &value.descriptor_digest},
						 std::pair{prefix + ".dependency_group_id", &value.dependency_group_id},
						 std::pair{prefix + ".atomic_output_group_id",
								   &value.atomic_output_group_id},
						 std::pair{prefix + ".batch_id", &value.batch_id},
						 std::pair{prefix + ".batch_digest", &value.batch_digest},
					 })
					if (auto valid = text(*member, field, false, limits, metadata_bytes); !valid)
						return unexpected(std::move(valid.error()));
				for (const auto& [field, member] : {
						 std::pair{prefix + ".descriptor_id", &value.descriptor_id},
						 std::pair{prefix + ".dependency_group_id", &value.dependency_group_id},
						 std::pair{prefix + ".atomic_output_group_id",
								   &value.atomic_output_group_id},
						 std::pair{prefix + ".batch_id", &value.batch_id},
					 })
					if (auto valid = strong_id(*member, field); !valid)
						return unexpected(std::move(valid.error()));
				if (!digest_like(value.descriptor_digest) || !digest_like(value.batch_digest))
					return unexpected(invalid(prefix, "digest"));
				if (value.row_count > maximum_rows - rows)
					return unexpected(limit("partitions.rows", "total"));
				rows += value.row_count;
			}

			static const std::set<std::string, std::less<>> coverage_states{
				"covered", "excluded", "failed", "not_applicable", "unresolved"};
			for (std::size_t index{}; index < draft.coverage.size(); ++index)
			{
				auto& value = draft.coverage[index];
				const auto prefix = "coverage[" + std::to_string(index) + "]";
				for (const auto& [field, member, allow_empty] : {
						 std::tuple{prefix + ".kind", &value.kind, false},
						 std::tuple{prefix + ".id", &value.id, false},
						 std::tuple{prefix + ".state", &value.state, false},
						 std::tuple{prefix + ".reason", &value.reason, true},
					 })
					if (auto valid = text(*member, field, allow_empty, limits, metadata_bytes);
						!valid)
						return unexpected(std::move(valid.error()));
				if (!coverage_states.contains(value.state))
					return unexpected(invalid(prefix + ".state", "closed-enum"));
			}

			for (std::size_t index{}; index < draft.unresolved.size(); ++index)
			{
				auto& value = draft.unresolved[index];
				const auto prefix = "unresolved[" + std::to_string(index) + "]";
				for (const auto& [field, member, allow_empty] : {
						 std::tuple{prefix + ".code", &value.code, false},
						 std::tuple{prefix + ".subject", &value.subject, false},
						 std::tuple{prefix + ".detail", &value.detail, true},
					 })
					if (auto valid = text(*member, field, allow_empty, limits, metadata_bytes);
						!valid)
						return unexpected(std::move(valid.error()));
				if (auto valid = validate_registered_symbol(value.code); !valid)
					return unexpected(invalid(prefix + ".code", "registered-symbol"));
			}

			for (std::size_t index{}; index < draft.provenance.size(); ++index)
			{
				auto& value = draft.provenance[index];
				const auto prefix = "provenance[" + std::to_string(index) + "]";
				for (const auto& [field, member, allow_empty] : {
						 std::tuple{prefix + ".kind", &value.kind, false},
						 std::tuple{prefix + ".subject", &value.subject, false},
						 std::tuple{prefix + ".producer", &value.producer, false},
						 std::tuple{prefix + ".summary", &value.summary, true},
					 })
					if (auto valid = text(*member, field, allow_empty, limits, metadata_bytes);
						!valid)
						return unexpected(std::move(valid.error()));
				if (auto valid = validate_registered_symbol(value.kind); !valid)
					return unexpected(invalid(prefix + ".kind", "registered-symbol"));
			}

			canonicalize_projections(draft);
			for (const auto duplicate : {
					 std::ranges::adjacent_find(draft.partitions, {}, partition_order) !=
						 draft.partitions.end(),
					 std::ranges::adjacent_find(draft.coverage, {}, coverage_order) !=
						 draft.coverage.end(),
					 std::ranges::adjacent_find(draft.unresolved) != draft.unresolved.end(),
					 std::ranges::adjacent_find(draft.provenance) != draft.provenance.end(),
				 })
				if (duplicate)
					return unexpected(invalid("projection", "duplicate"));

			for (const auto& [field, value] : {
					 std::pair{std::string{"run_authentication.algorithm"},
							   &draft.authentication.algorithm},
					 std::pair{std::string{"run_authentication.signer_id"},
							   &draft.authentication.signer_id},
					 std::pair{std::string{"run_authentication.key_fingerprint"},
							   &draft.authentication.key_fingerprint},
					 std::pair{std::string{"run_authentication.signed_subject_digest"},
							   &draft.authentication.signed_subject_digest},
					 std::pair{std::string{"run_authentication.signature_digest"},
							   &draft.authentication.signature_digest},
				 })
				if (auto valid = text(*value, field, false, limits, metadata_bytes); !valid)
					return unexpected(std::move(valid.error()));
			if (draft.authentication.algorithm != "ed25519")
				return unexpected(invalid("run_authentication.algorithm", "unsupported"));
			if (auto valid =
					strong_id(draft.authentication.signer_id, "run_authentication.signer_id");
				!valid)
				return unexpected(std::move(valid.error()));
			for (const auto* value : {&draft.authentication.key_fingerprint,
									  &draft.authentication.signed_subject_digest,
									  &draft.authentication.signature_digest})
				if (!digest_like(*value))
					return unexpected(invalid("run_authentication", "digest"));
			const std::vector<std::byte> signature{draft.authentication.signature.begin(),
												   draft.authentication.signature.end()};
			if (content_digest(signature) != draft.authentication.signature_digest)
				return unexpected(invalid("run_authentication.signature", "digest-mismatch"));
			auto signed_subject = detached_provider_run_signed_subject_digest(draft);
			if (!signed_subject || *signed_subject != draft.authentication.signed_subject_digest)
				return unexpected(invalid("run_authentication", "subject-mismatch"));

			const bool adoptable = draft.terminal == detached_provider_terminal::complete ||
				draft.terminal == detached_provider_terminal::partial;
			if (!adoptable && (!draft.partitions.empty() || draft.runtime_receipt_digest))
				return unexpected(invalid("terminal", "publication-candidate-forbidden"));
			if (draft.terminal == detached_provider_terminal::complete && draft.partitions.empty())
				return unexpected(invalid("terminal", "empty-complete"));
			if (draft.terminal == detached_provider_terminal::partial && draft.partitions.empty() &&
				draft.unresolved.empty())
				return unexpected(invalid("terminal", "empty-partial"));

			auto encoded = encode(draft);
			if (!encoded || encoded->size() > limits.maximum_bundle_bytes)
				return unexpected(limit("detached_provider_run", "bytes"));
			auto digest = content_digest(*encoded);
			return validated_detached_provider_run{
				std::move(draft), std::move(*encoded), std::move(digest)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("detached_provider_run", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("detached_provider_run", "allocation-length"));
		}
	}

	result<validated_detached_provider_run>
	decode_detached_provider_run(const std::span<const std::byte> bytes, const import_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return unexpected(std::move(valid.error()));
			if (bytes.empty() || bytes.size() > limits.maximum_bundle_bytes)
				return unexpected(limit("detached_provider_run", "bytes"));
			auto root = decode_bounded_canonical_binary(
				bytes,
				{.initial_depth = 0U,
				 .maximum_nesting_depth = limits.maximum_nesting_depth,
				 .invalid_error_code = "application-analysis.detached-provider-run-invalid",
				 .limit_error_code = "application-analysis.import-limit-exceeded",
				 .maximum_tuple_items = maximum_diagnostics,
				 .maximum_total_values = maximum_canonical_values});
			if (!root)
				return unexpected(std::move(root.error()));
			auto fields = tuple(*root, "root", 13U);
			if (!fields)
				return unexpected(std::move(fields.error()));
			auto schema = string((**fields)[0], "schema");
			if (!schema || *schema != "cxxlens.detached-provider-run.v1")
				return unexpected(invalid("schema", "unsupported"));

			detached_provider_run_draft draft;
			auto assign = [&](const std::size_t index,
							  const std::string& field,
							  std::string& destination) -> result<void>
			{
				auto decoded = string((**fields)[index], field);
				if (!decoded)
					return unexpected(std::move(decoded.error()));
				destination = std::move(*decoded);
				return {};
			};
			for (const auto& [index, field, destination] : {
					 std::tuple{1U, std::string{"task_id"}, &draft.task_id},
					 std::tuple{2U, std::string{"task_input_digest"}, &draft.task_input_digest},
					 std::tuple{3U, std::string{"replay_plan_digest"}, &draft.replay_plan_digest},
				 })
				if (auto valid = assign(index, field, *destination); !valid)
					return unexpected(std::move(valid.error()));

			auto identity = tuple((**fields)[4], "provider_identity", 7U);
			if (!identity)
				return unexpected(std::move(identity.error()));
			for (const auto& [index, field, destination] : {
					 std::tuple{0U, std::string{"provider_id"}, &draft.provider.provider_id},
					 std::tuple{2U, std::string{"binary_digest"}, &draft.provider.binary_digest},
					 std::tuple{3U,
								std::string{"semantic_contract_digest"},
								&draft.provider.semantic_contract_digest},
					 std::tuple{
						 4U, std::string{"signature_digest"}, &draft.provider.signature_digest},
					 std::tuple{
						 5U, std::string{"revocation_state"}, &draft.provider.revocation_state},
					 std::tuple{6U,
								std::string{"sandbox_policy_digest"},
								&draft.provider.sandbox_policy_digest},
				 })
			{
				auto decoded = string((**identity)[index], field);
				if (!decoded)
					return unexpected(std::move(decoded.error()));
				*destination = std::move(*decoded);
			}
			auto provider_version = version((**identity)[1]);
			if (!provider_version)
				return unexpected(std::move(provider_version.error()));
			draft.provider.provider_version = *provider_version;
			if ((**fields)[5].type != canonical_value::kind::bytes)
				return unexpected(invalid("protocol_transcript", "bytes"));
			draft.protocol_transcript = (**fields)[5].byte_string;
			auto terminal_text = string((**fields)[6], "terminal");
			if (!terminal_text)
				return unexpected(std::move(terminal_text.error()));
			auto terminal = terminal_from(*terminal_text);
			if (!terminal)
				return unexpected(std::move(terminal.error()));
			draft.terminal = *terminal;

			auto partitions = decode_collection<detached_partition_projection>(
				(**fields)[7],
				"partitions",
				maximum_partitions,
				[](const canonical_value& item,
				   const std::string& field) -> result<detached_partition_projection>
				{
					auto values = tuple(item, field, 7U);
					if (!values)
						return unexpected(std::move(values.error()));
					detached_partition_projection output;
					std::array<std::string*, 6U> destinations{&output.descriptor_id,
															  &output.descriptor_digest,
															  &output.dependency_group_id,
															  &output.atomic_output_group_id,
															  &output.batch_id,
															  &output.batch_digest};
					auto value = (*values)->begin();
					for (auto* destination : destinations)
					{
						auto decoded = string(*value, field);
						if (!decoded)
							return unexpected(std::move(decoded.error()));
						*destination = std::move(*decoded);
						++value;
					}
					auto rows = unsigned_integer((**values)[6], field + ".row_count");
					if (!rows)
						return unexpected(std::move(rows.error()));
					output.row_count = *rows;
					return output;
				});
			if (!partitions)
				return unexpected(std::move(partitions.error()));
			draft.partitions = std::move(*partitions);

			auto coverage = decode_collection<detached_coverage_projection>(
				(**fields)[8],
				"coverage",
				maximum_diagnostics,
				[](const canonical_value& item,
				   const std::string& field) -> result<detached_coverage_projection>
				{
					auto values = tuple(item, field, 4U);
					if (!values)
						return unexpected(std::move(values.error()));
					detached_coverage_projection output;
					std::array<std::string*, 4U> destinations{
						&output.kind, &output.id, &output.state, &output.reason};
					auto value = (*values)->begin();
					for (auto* destination : destinations)
					{
						auto decoded = string(*value, field);
						if (!decoded)
							return unexpected(std::move(decoded.error()));
						*destination = std::move(*decoded);
						++value;
					}
					return output;
				});
			if (!coverage)
				return unexpected(std::move(coverage.error()));
			draft.coverage = std::move(*coverage);

			auto unresolved = decode_collection<detached_unresolved_projection>(
				(**fields)[9],
				"unresolved",
				maximum_diagnostics,
				[](const canonical_value& item,
				   const std::string& field) -> result<detached_unresolved_projection>
				{
					auto values = tuple(item, field, 3U);
					if (!values)
						return unexpected(std::move(values.error()));
					detached_unresolved_projection output;
					std::array<std::string*, 3U> destinations{
						&output.code, &output.subject, &output.detail};
					auto value = (*values)->begin();
					for (auto* destination : destinations)
					{
						auto decoded = string(*value, field);
						if (!decoded)
							return unexpected(std::move(decoded.error()));
						*destination = std::move(*decoded);
						++value;
					}
					return output;
				});
			if (!unresolved)
				return unexpected(std::move(unresolved.error()));
			draft.unresolved = std::move(*unresolved);

			auto provenance = decode_collection<detached_provenance_projection>(
				(**fields)[10],
				"provenance",
				maximum_diagnostics,
				[](const canonical_value& item,
				   const std::string& field) -> result<detached_provenance_projection>
				{
					auto values = tuple(item, field, 4U);
					if (!values)
						return unexpected(std::move(values.error()));
					detached_provenance_projection output;
					std::array<std::string*, 4U> destinations{
						&output.kind, &output.subject, &output.producer, &output.summary};
					auto value = (*values)->begin();
					for (auto* destination : destinations)
					{
						auto decoded = string(*value, field);
						if (!decoded)
							return unexpected(std::move(decoded.error()));
						*destination = std::move(*decoded);
						++value;
					}
					return output;
				});
			if (!provenance)
				return unexpected(std::move(provenance.error()));
			draft.provenance = std::move(*provenance);

			if ((**fields)[11].type != canonical_value::kind::null_value)
			{
				auto receipt = string((**fields)[11], "runtime_receipt_digest");
				if (!receipt)
					return unexpected(std::move(receipt.error()));
				draft.runtime_receipt_digest = std::move(*receipt);
			}
			auto authentication = tuple((**fields)[12], "run_authentication", 6U);
			if (!authentication || (**authentication)[4].type != canonical_value::kind::bytes ||
				(**authentication)[4].byte_string.size() != detached_provider_run_signature_bytes)
				return unexpected(invalid("run_authentication", "tuple-shape"));
			for (const auto& [index, field, destination] : {
					 std::tuple{0U, std::string{"algorithm"}, &draft.authentication.algorithm},
					 std::tuple{1U, std::string{"signer_id"}, &draft.authentication.signer_id},
					 std::tuple{
						 2U, std::string{"key_fingerprint"}, &draft.authentication.key_fingerprint},
					 std::tuple{3U,
								std::string{"signed_subject_digest"},
								&draft.authentication.signed_subject_digest},
					 std::tuple{5U,
								std::string{"signature_digest"},
								&draft.authentication.signature_digest},
				 })
			{
				auto decoded = string((**authentication)[index], field);
				if (!decoded)
					return unexpected(std::move(decoded.error()));
				*destination = std::move(*decoded);
			}
			std::ranges::copy((**authentication)[4].byte_string,
							  draft.authentication.signature.begin());

			auto validated = validate_detached_provider_run(std::move(draft), limits);
			if (!validated)
				return unexpected(std::move(validated.error()));
			if (!std::ranges::equal(bytes, validated->bytes()))
				return unexpected(invalid("binary", "noncanonical-projection-order"));
			return validated;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("detached_provider_run", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("detached_provider_run", "allocation-length"));
		}
	}
} // namespace cxxlens::sdk::detail
