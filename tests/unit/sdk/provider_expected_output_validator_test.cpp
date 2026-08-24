#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sdk/provider_expected_output_validator_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;
	using namespace cxxlens::sdk::provider::detail;

	constexpr std::string_view task_id =
		"task:semantic-v2:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	[[nodiscard]] std::string digest(const char digit)
	{
		return "semantic-v2:sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] relation_descriptor descriptor_for(const std::string_view id)
	{
		relation_descriptor descriptor;
		descriptor.id = id;
		const auto version_marker = id.rfind(".v");
		descriptor.name = std::string{id.substr(0U, version_marker)};
		descriptor.version = version_marker != std::string_view::npos && id.ends_with(".v2")
			? semantic_version{2U, 0U, 0U}
			: semantic_version{1U, 0U, 0U};
		descriptor.semantic_major = descriptor.version.major;
		descriptor.semantics = "test.relation/" + std::to_string(descriptor.semantic_major);
		descriptor.owner_namespace = "cxxlens.test";
		const auto column_id = descriptor.id + ".value";
		descriptor.columns = {{column_id,
							   "value",
							   {scalar_kind::utf8_string, "", false},
							   true,
							   column_role::claim_key}};
		descriptor.key_columns = {column_id};
		auto descriptor_digest = semantic_digest("cxxlens.relation-descriptor-binding.v2",
												 "\n" + descriptor.canonical_form());
		require(static_cast<bool>(descriptor_digest), "synthetic descriptor digest");
		descriptor.descriptor_digest = std::move(*descriptor_digest);
		return descriptor;
	}

	[[nodiscard]] std::vector<relation_descriptor> descriptors()
	{
		std::vector<relation_descriptor> output;
		output.reserve(expected_output_descriptor_count);
		for (std::size_t index{}; index < expected_output_descriptor_count; ++index)
			output.push_back(descriptor_for(expected_output_descriptor_ids[index]));
		return output;
	}

	[[nodiscard]] expected_output_authority
	authority(const std::span<const relation_descriptor> relation_descriptors)
	{
		expected_output_authority output;
		output.task_id = std::string{task_id};
		for (std::size_t index{}; index < relation_descriptors.size(); ++index)
			output.batches.push_back({std::string{relation_descriptors[index].id},
									  relation_descriptors[index].descriptor_digest,
									  index < 3U ? "canonical" : "observation",
									  "clang22-atomic",
									  relation_descriptors[index].id + "-batch"});
		return output;
	}

	[[nodiscard]] frame frame_with(const message_type type,
								   const std::uint64_t sequence,
								   std::vector<std::byte> control = {},
								   std::vector<std::byte> payload = {},
								   const std::uint16_t flags = 0U)
	{
		return frame{type,
					 1U,
					 sequence,
					 std::move(control),
					 std::move(payload),
					 protocol_v2_major,
					 protocol_v2_minor,
					 flags};
	}

	void append(std::vector<frame>& frames,
				const message_type type,
				std::vector<std::byte> control = {},
				std::vector<std::byte> payload = {},
				const std::uint16_t flags = 0U)
	{
		frames.push_back(
			frame_with(type, frames.size(), std::move(control), std::move(payload), flags));
	}

	[[nodiscard]] std::vector<std::byte> take(result<std::vector<std::byte>> value,
											  const std::string_view message)
	{
		require(value.has_value(), message);
		return std::move(*value);
	}

	[[nodiscard]] std::vector<frame>
	transcript(const std::span<const relation_descriptor> descriptors)
	{
		std::vector<frame> frames;
		append(frames, message_type::hello, take(encode_control_text(""), "hello encoding"));
		append(frames,
			   message_type::schema_negotiate,
			   take(encode_schema_negotiate_metadata({"cxxlens.provider-protocol.v2", 0U}),
					"schema encoding"));
		append(frames,
			   message_type::task_accepted,
			   take(encode_task_accepted_metadata({"provider.test", "1.0.0", std::string{task_id}}),
					"accepted encoding"));
		for (std::size_t index{}; index < descriptors.size(); ++index)
		{
			const auto dependency = index < 3U ? "canonical" : "observation";
			const auto batch_id = descriptors[index].id + "-batch";
			append(frames,
				   message_type::batch_begin,
				   take(encode_batch_begin_metadata({std::string{task_id},
													 descriptors[index].id,
													 descriptors[index].descriptor_digest,
													 dependency,
													 "clang22-atomic",
													 batch_id}),
						"batch-begin encoding"));
			columnar_batch_end end{std::string{task_id},
								   dependency,
								   "clang22-atomic",
								   batch_id,
								   descriptors[index].id,
								   descriptors[index].descriptor_digest,
								   0U,
								   {{descriptors[index].columns.front().id, 0U, 0U}},
								   {},
								   {}};
			end.batch_digest = columnar_batch_digest(end);
			auto encoded = encode_columnar_batch_end(end);
			require(static_cast<bool>(encoded), "batch-end encoding");
			append(frames,
				   message_type::batch_end,
				   std::move(encoded->control),
				   std::move(encoded->payload));
		}
		const std::vector<coverage_unit> coverage{{"task", std::string{task_id}, "covered", {}}};
		append(frames,
			   message_type::coverage_chunk,
			   take(encode_coverage_metadata(coverage), "coverage encoding"));
		append(frames,
			   message_type::unresolved_chunk,
			   take(encode_unresolved_metadata(std::vector<unresolved_item>{}),
					"unresolved encoding"));
		const std::vector<evidence_item> evidence{
			{"provider.progress", std::string{task_id}, "provider.test", "complete"}};
		append(frames,
			   message_type::progress,
			   take(encode_evidence_metadata(evidence), "progress encoding"));
		append(frames,
			   message_type::task_complete,
			   take(encode_task_complete_metadata({std::string{task_id}}), "complete encoding"),
			   {},
			   static_cast<std::uint16_t>(frame_flag::end_of_stream));
		return frames;
	}

	[[nodiscard]] transcript_validation_request
	request(const std::span<const relation_descriptor> descriptors)
	{
		transcript_validation_request output;
		output.task_id = std::string{task_id};
		output.provider_id = "provider.test";
		output.provider_version = {1U, 0U, 0U};
		output.output_descriptors = descriptors;
		output.output_credit = {64U * 1024U * 1024U, 128U};
		output.require_handshake = true;
		return output;
	}

	void renumber(std::vector<frame>& frames)
	{
		for (std::size_t index{}; index < frames.size(); ++index)
			frames[index].sequence = index;
	}

	void positive_exact_six_including_empty()
	{
		auto descriptors_value = descriptors();
		auto expected = authority(descriptors_value);
		auto frames = transcript(descriptors_value);
		auto sealed = validate_expected_output_transcript(
			std::move(expected), request(descriptors_value), frames, {});
		require(static_cast<bool>(sealed), "exact six empty transcript was rejected");
		require(sealed->authority().batches.size() == expected_output_descriptor_count,
				"expected output authority was not retained");
		require(sealed->output().batches().size() == expected_output_descriptor_count,
				"empty batches were not retained");
		for (const auto& batch : sealed->output().batches())
		{
			require(batch.rows().empty() && batch.ordered_chunk_digests().empty(),
					"empty batch was not losslessly sealed");
			require(batch.columns().size() == 1U && batch.columns().front().chunk_count == 0U,
					"empty column summary was not retained");
		}
		require(sealed->output().coverage().size() == 1U && sealed->output().unresolved().empty() &&
					sealed->output().evidence().size() == 1U,
				"side channels were not retained");
	}

	void reject_partial_authority_and_binding()
	{
		auto descriptors_value = descriptors();
		auto expected = authority(descriptors_value);
		expected.batches.pop_back();
		auto rejected = validate_expected_output_transcript(
			std::move(expected), request(descriptors_value), transcript(descriptors_value), {});
		require(!rejected && rejected.error().detail == "exact-six-required",
				"partial expected set was accepted");

		expected = authority(descriptors_value);
		expected.batches.front().runtime_descriptor_digest = digest('f');
		rejected = validate_expected_output_transcript(
			std::move(expected), request(descriptors_value), transcript(descriptors_value), {});
		require(!rejected && rejected.error().detail == "runtime-descriptor-mismatch",
				"runtime descriptor mismatch was accepted");

		expected = authority(descriptors_value);
		expected.batches.front().dependency_group_id = "observation";
		rejected = validate_expected_output_transcript(
			std::move(expected), request(descriptors_value), transcript(descriptors_value), {});
		require(!rejected && rejected.error().detail == "canonical-group-binding",
				"partial dependency group authority was accepted");
	}

	void reject_reserved_candidate_and_output_after_terminal()
	{
		auto descriptors_value = descriptors();
		auto expected = authority(descriptors_value);
		auto frames = transcript(descriptors_value);
		frames.insert(frames.begin() + 3U,
					  frame_with(message_type::closure_candidate,
								 3U,
								 {},
								 {},
								 static_cast<std::uint16_t>(frame_flag::optional_extension)));
		renumber(frames);
		auto rejected = validate_expected_output_transcript(
			std::move(expected), request(descriptors_value), frames, {});
		require(!rejected && rejected.error().detail == "closure-candidate-forbidden",
				"reserved closure-candidate frame was accepted");

		expected = authority(descriptors_value);
		frames = transcript(descriptors_value);
		frames.push_back(frame_with(message_type::progress,
									frames.size(),
									{},
									{},
									static_cast<std::uint16_t>(frame_flag::optional_extension)));
		auto after_terminal = validate_expected_output_transcript(
			std::move(expected), request(descriptors_value), frames, {});
		require(!after_terminal && after_terminal.error().detail == "output-after-terminal",
				"output after terminal was accepted");
	}
} // namespace

int main()
{
	positive_exact_six_including_empty();
	reject_partial_authority_and_binding();
	reject_reserved_candidate_and_output_after_terminal();
	return EXIT_SUCCESS;
}
