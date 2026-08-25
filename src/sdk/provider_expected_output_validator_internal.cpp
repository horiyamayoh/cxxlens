#include "provider_expected_output_validator_internal.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {"provider.expected-output-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool lower_hex(const std::string_view value) noexcept
		{
			return !value.empty() &&
				std::ranges::all_of(value,
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool protocol_digest(const std::string_view value) noexcept
		{
			constexpr std::string_view raw_prefix{"sha256:"};
			constexpr std::string_view semantic_prefix{"semantic-v2:sha256:"};
			if (value.starts_with(raw_prefix))
				return value.size() == raw_prefix.size() + 64U &&
					lower_hex(value.substr(raw_prefix.size()));
			if (value.starts_with(semantic_prefix))
				return value.size() == semantic_prefix.size() + 64U &&
					lower_hex(value.substr(semantic_prefix.size()));
			return false;
		}

		[[nodiscard]] bool terminal_type(const message_type type) noexcept
		{
			return type == message_type::task_complete || type == message_type::task_failed;
		}

		[[nodiscard]] bool expected_group(const std::size_t index,
										  const std::string_view value) noexcept
		{
			return value == (index < 3U ? "canonical" : "observation");
		}

		[[nodiscard]] result<void>
		validate_expected_batch(const expected_output_batch_binding& batch,
								const std::size_t index,
								const std::string_view task_id)
		{
			if (batch.descriptor_id != expected_output_descriptor_ids[index])
				return cxxlens::sdk::unexpected(
					invalid("batches.descriptor_id", "canonical-order"));
			if (auto valid = validate_strong_id(batch.descriptor_id); !valid)
				return cxxlens::sdk::unexpected(invalid("batches.descriptor_id", "invalid-id"));
			if (!protocol_digest(batch.runtime_descriptor_digest))
				return cxxlens::sdk::unexpected(
					invalid("batches.runtime_descriptor_digest", "semantic-digest"));
			if (!expected_group(index, batch.dependency_group_id))
				return cxxlens::sdk::unexpected(
					invalid("batches.dependency_group_id", "canonical-group-binding"));
			if (batch.atomic_output_group_id != "clang22-atomic")
				return cxxlens::sdk::unexpected(
					invalid("batches.atomic_output_group_id", "atomic-group-binding"));
			if (batch.batch_id != batch.descriptor_id + "-batch")
				return cxxlens::sdk::unexpected(
					invalid("batches.batch_id", "descriptor-batch-binding"));
			if (task_id.empty() || task_id.contains('\0'))
				return cxxlens::sdk::unexpected(invalid("task_id", "invalid-id"));
			return {};
		}

		[[nodiscard]] result<void> compare_batch(const sealed_provider_batch& actual,
												 const expected_output_batch_binding& expected,
												 const std::string_view task_id)
		{
			if (actual.task_id() != task_id || actual.descriptor_id() != expected.descriptor_id)
				return cxxlens::sdk::unexpected(invalid("sealed.batch", "task-or-descriptor"));
			if (actual.descriptor_digest() != expected.runtime_descriptor_digest)
				return cxxlens::sdk::unexpected(
					invalid("sealed.batch.descriptor_digest", "runtime-descriptor-mismatch"));
			if (actual.dependency_group_id() != expected.dependency_group_id ||
				actual.atomic_output_group_id() != expected.atomic_output_group_id ||
				actual.batch_id() != expected.batch_id)
				return cxxlens::sdk::unexpected(invalid("sealed.batch", "group-or-batch-binding"));
			if (!protocol_digest(actual.batch_digest()))
				return cxxlens::sdk::unexpected(invalid("sealed.batch.batch_digest", "digest"));

			// The existing shared validator checks these invariants while decoding.  Repeat the
			// empty-batch projection at this host-authority boundary so an expected empty relation
			// cannot be silently omitted or represented by a nonempty chunk set.
			const auto rows_empty = actual.rows().empty();
			const auto chunks_empty = actual.ordered_chunk_digests().empty();
			if (rows_empty != chunks_empty)
				return cxxlens::sdk::unexpected(invalid("sealed.batch", "empty-projection"));
			for (const auto& column : actual.columns())
				if (rows_empty != (column.chunk_count == 0U))
					return cxxlens::sdk::unexpected(
						invalid("sealed.batch.columns", "empty-projection"));
			return {};
		}
	} // namespace

	result<void> expected_output_authority::validate() const
	{
		if (task_id.empty() || task_id.contains('\0') || !validate_strong_id(task_id))
			return cxxlens::sdk::unexpected(invalid("task_id", "invalid-id"));
		if (batches.size() != expected_output_descriptor_count)
			return cxxlens::sdk::unexpected(invalid("batches", "exact-six-required"));
		std::set<std::string, std::less<>> batch_ids;
		std::set<std::string, std::less<>> descriptor_ids;
		for (std::size_t index{}; index < batches.size(); ++index)
		{
			if (auto valid = validate_expected_batch(batches[index], index, task_id); !valid)
				return valid;
			if (!descriptor_ids.insert(batches[index].descriptor_id).second ||
				!batch_ids.insert(batches[index].batch_id).second)
				return cxxlens::sdk::unexpected(invalid("batches", "duplicate"));
		}
		// The expected six form two complete dependency groups.  This explicit census keeps a
		// malformed host authority from authorizing a partial group even when frame count matches.
		const auto canonical =
			std::ranges::count_if(batches,
								  [](const auto& value)
								  {
									  return value.dependency_group_id == "canonical";
								  });
		const auto observation =
			std::ranges::count_if(batches,
								  [](const auto& value)
								  {
									  return value.dependency_group_id == "observation";
								  });
		if (canonical != 3 || observation != 3)
			return cxxlens::sdk::unexpected(invalid("batches", "partial-dependency-group"));
		return {};
	}

	result<expected_output_sealed_transcript>
	validate_expected_output_transcript(expected_output_authority expected,
										const transcript_validation_request& request,
										const std::span<const frame> frames,
										const protocol_limits session_limits)
	{
		if (auto valid = expected.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (request.task_id != expected.task_id)
			return cxxlens::sdk::unexpected(invalid("task_id", "request-mismatch"));
		if (request.output_descriptors.size() != expected_output_descriptor_count)
			return cxxlens::sdk::unexpected(invalid("output_descriptors", "exact-six-required"));
		for (std::size_t index{}; index < expected.batches.size(); ++index)
		{
			const auto& descriptor = request.output_descriptors[index];
			const auto& binding = expected.batches[index];
			if (descriptor.id != binding.descriptor_id ||
				descriptor.descriptor_digest != binding.runtime_descriptor_digest)
				return cxxlens::sdk::unexpected(
					invalid("output_descriptors", "runtime-descriptor-mismatch"));
			if (auto valid = descriptor.validate(); !valid)
				return cxxlens::sdk::unexpected(
					invalid("output_descriptors", "descriptor-invalid"));
		}

		// The shared validator deliberately skips optional extensions before its output switch.  An
		// output authority must not allow the reserved closure-candidate ID to hide in that path,
		// and no frame may appear after either terminal marker, including an optional extension.
		bool terminal_seen{};
		for (const auto& value : frames)
		{
			if (value.type == message_type::closure_candidate)
				return cxxlens::sdk::unexpected(invalid("frames", "closure-candidate-forbidden"));
			if (terminal_seen)
				return cxxlens::sdk::unexpected(invalid("frames", "output-after-terminal"));
			if (terminal_type(value.type))
				terminal_seen = true;
		}

		auto validated = validate_provider_transcript(request, frames, session_limits);
		if (!validated)
			return cxxlens::sdk::unexpected(std::move(validated.error()));
		if (validated->kind != transcript_terminal_kind::complete || !validated->sealed())
			return cxxlens::sdk::unexpected(invalid("frames", "complete-sealed-output-required"));

		auto sealed = std::move(*validated).take_sealed();
		if (!sealed)
			return cxxlens::sdk::unexpected(invalid("frames", "complete-sealed-output-required"));
		if (sealed->batches().size() != expected_output_descriptor_count)
			return cxxlens::sdk::unexpected(invalid("sealed.batches", "exact-six-required"));
		for (std::size_t index{}; index < expected.batches.size(); ++index)
		{
			if (auto valid = compare_batch(
					sealed->batches()[index], expected.batches[index], expected.task_id);
				!valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
		}
		return expected_output_sealed_transcript{std::move(expected), std::move(*sealed)};
	}
} // namespace cxxlens::sdk::provider::detail
