#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>

#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/observation_v2.hpp"
#include "llvm/clang22/provider_task_v4.hpp"
#include "llvm/clang22/provider_worker_ingress.hpp"
#include "llvm/clang22/provider_worker_protocol_v2_input.hpp"

namespace
{
	using namespace cxxlens::detail::clang22;
	using cxxlens::detail::clang22::materialization::canonical_json;
	using cxxlens::detail::clang22::materialization::json_value;
	using cxxlens::detail::clang22::materialization::parse_json_object;
	using cxxlens::detail::clang22::materialization::utf8_byte_less;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::abort();
		}
	}

	[[nodiscard]] std::string semantic(const char digit)
	{
		return "semantic-v2:sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::string content(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] json_value text(const std::string_view value)
	{
		auto output = json_value::string(std::string{value});
		require(output.has_value(), "test text encoding failed");
		return std::move(*output);
	}

	[[nodiscard]] json_value object(json_value::object_type fields)
	{
		auto output = json_value::object(std::move(fields));
		require(output.has_value(), "test object encoding failed");
		return std::move(*output);
	}

	[[nodiscard]] json_value array(std::vector<json_value> values)
	{
		return json_value::array(std::move(values));
	}

	struct fixture
	{
		std::string envelope;
		std::string task_id;
		std::string task_digest;
		std::string input_digest;
		std::string base_digest;
	};

	[[nodiscard]] fixture make_fixture()
	{
		const std::string working_directory{"project://src"};
		const std::string main_path{"project://src/main.cpp"};
		const std::vector<std::string> arguments{
			"/usr/bin/clang++", "-nostdinc", "-nostdinc++", main_path};
		const auto invocation =
			derive_provider_task_v4_effective_invocation_digest(working_directory, arguments);
		require(invocation.has_value(), "test invocation digest failed");

		const std::string task_digest = semantic('a');
		const std::string task_id = "task:" + task_digest;
		const std::string closure_digest = semantic('b');
		const std::string closure_id = "source-closure:" + closure_digest;
		const std::string manifest_digest = semantic('c');
		const std::string session_id = "provider-session:sha256:" + std::string(64U, 'd');
		const std::string transfer_digest = semantic('e');

		auto base_projection =
			object({{"schema", text("base-task.v2")}, {"value", text("explicit")}});
		const auto base_text = canonical_json(base_projection);
		const auto base_bytes = std::as_bytes(std::span{base_text.data(), base_text.size()});
		const std::string base_digest = cxxlens::sdk::content_digest(base_bytes);

		auto open_task = object({
			{"environment_digest", text(content('f'))},
			{"normalized_invocation_digest", text(*invocation)},
			{"task_input_digest", text(content('1'))},
			{"toolchain_digest", text(semantic('2'))},
		});
		auto source_closure = object({{"digest", text(closure_digest)},
									  {"id", text(closure_id)},
									  {"manifest_digest", text(manifest_digest)}});
		auto task_payload = object({
			{"base_provider_task_id", text("task:semantic-v2:sha256:" + std::string(64U, '3'))},
			{"base_task_digest", text(base_digest)},
			{"base_task_index", json_value::unsigned_integer(0U)},
			{"logical_working_directory", text(working_directory)},
			{"main_logical_path", text(main_path)},
			{"open_task", std::move(open_task)},
			{"schema", text(provider_task_v4_schema)},
			{"source_closure", std::move(source_closure)},
			{"task_id", text(task_id)},
			{"task_v4_digest", text(task_digest)},
		});
		const auto payload_text = canonical_json(task_payload);
		const auto payload_bytes =
			std::as_bytes(std::span{payload_text.data(), payload_text.size()});
		const std::string input_digest = cxxlens::sdk::content_digest(payload_bytes);

		std::vector<json_value> argument_values;
		for (const auto& argument : arguments)
			argument_values.push_back(text(argument));
		std::vector<json_value> root_values{text("/usr")};
		const std::array<const cxxlens::sdk::relation_descriptor*, 6U> descriptors{
			&cxxlens::cc::relations::call_direct_target::descriptor(),
			&cxxlens::cc::relations::call_site::descriptor(),
			&cxxlens::cc::relations::entity::descriptor(),
			&materialization::call_observation_v2_descriptor(),
			&materialization::entity_observation_v2_descriptor(),
			&materialization::type_observation_v2_descriptor()};
		std::vector<json_value> descriptor_ids;
		std::vector<json_value> descriptor_digests;
		for (std::size_t index{}; index < descriptors.size(); ++index)
		{
			descriptor_ids.push_back(text(std::string{task_v4_output_descriptor_ids[index]}));
			descriptor_digests.push_back(text(descriptors[index]->descriptor_digest));
		}
		std::vector<json_value> dependency_groups;
		for (const auto group : task_v4_dependency_groups)
			dependency_groups.push_back(text(group));
		auto output_authority = object({
			{"compile_unit_id", text("compile-unit:one")},
			{"dependency_groups", array(std::move(dependency_groups))},
			{"descriptor_digests", array(std::move(descriptor_digests))},
			{"maximum_output_bytes", json_value::unsigned_integer(16U * 1024U * 1024U)},
			{"maximum_rows", json_value::unsigned_integer(100000U)},
			{"provider_id", text("provider:clang22")},
			{"provider_version", text("1.0.0")},
			{"requested_descriptor_ids", array(std::move(descriptor_ids))},
			{"semantic_contract_digest", text(semantic('4'))},
			{"toolchain_context_id", text("toolchain-context:one")},
		});
		auto authority = object({
			{"effective_arguments", array(std::move(argument_values))},
			{"logical_working_directory", text(working_directory)},
			{"normalized_invocation_digest", text(*invocation)},
			{"qualified_read_roots", array(std::move(root_values))},
		});
		auto closure_binding = object({
			{"closure_digest", text(closure_digest)},
			{"closure_id", text(closure_id)},
			{"expected_transfer_digest", text(transfer_digest)},
			{"first_sequence", json_value::unsigned_integer(0U)},
			{"manifest_digest", text(manifest_digest)},
			{"session_id", text(session_id)},
			{"stream_id", json_value::unsigned_integer(7U)},
			{"task_id", text(task_id)},
			{"task_v4_digest", text(task_digest)},
		});
		auto root = object({
			{"base_task_projection", std::move(base_projection)},
			{"closure_binding", std::move(closure_binding)},
			{"expected_base_task_digest", text(base_digest)},
			{"expected_task_v4_input_digest", text(input_digest)},
			{"input_authority", std::move(authority)},
			{"output_authority", std::move(output_authority)},
			{"schema", text("cxxlens.clang22.worker-ingress.v4")},
			{"stream_id", json_value::unsigned_integer(7U)},
			{"task_v4_payload", std::move(task_payload)},
		});
		return {canonical_json(root), task_id, task_digest, input_digest, base_digest};
	}

	[[nodiscard]] std::string canonical_with_root(const fixture& input,
												  const std::string_view field,
												  const json_value& replacement)
	{
		auto document = parse_json_object(input.envelope);
		require(document.has_value(), "fixture envelope parse failed");
		auto fields = *document->root().as_object();
		fields.insert_or_assign(std::string{field}, replacement);
		return canonical_json(object(std::move(fields)));
	}

	void positive_decodes_all_authority()
	{
		auto input = make_fixture();
		auto decoded = decode_provider_worker_v4_ingress(input.envelope);
		require(decoded.has_value(), "explicit task-v4 envelope was rejected");
		require(decoded->closure_binding.task_id == input.task_id &&
					decoded->closure_binding.task_v4_digest == input.task_digest &&
					decoded->expected_task_v4_input_digest == input.input_digest &&
					decoded->expected_base_task_digest == input.base_digest &&
					decoded->input_authority.effective_arguments.size() == 4U &&
					decoded->input_authority.qualified_read_roots ==
						std::vector<std::string>{"/usr"},
				"typed authority did not retain explicit fields");
	}

	void negative_rejects_noncanonical_and_missing_fields()
	{
		auto input = make_fixture();
		auto noncanonical = std::string{" "} + input.envelope;
		auto rejected = decode_provider_worker_v4_ingress(std::move(noncanonical));
		require(!rejected && rejected.error().detail == "noncanonical",
				"noncanonical envelope was accepted");

		auto document = parse_json_object(input.envelope);
		require(document.has_value(), "fixture envelope parse failed");
		auto fields = *document->root().as_object();
		fields.erase("input_authority");
		auto missing = decode_provider_worker_v4_ingress(canonical_json(object(std::move(fields))));
		require(!missing && missing.error().detail == "field-census",
				"missing explicit input authority was accepted");
	}

	void negative_rejects_identity_and_source_bytes()
	{
		auto input = make_fixture();
		auto document = parse_json_object(input.envelope);
		require(document.has_value(), "fixture envelope parse failed");
		auto fields = *document->root().as_object();
		auto closure = *fields.at("closure_binding").as_object();
		closure.insert_or_assign("task_id",
								 text("task:semantic-v2:sha256:" + std::string(64U, 'f')));
		fields.insert_or_assign("closure_binding", object(std::move(closure)));
		auto foreign = decode_provider_worker_v4_ingress(canonical_json(object(std::move(fields))));
		require(!foreign && foreign.error().code == "source-closure.task-binding-mismatch",
				"foreign closure identity was accepted");

		document = parse_json_object(input.envelope);
		require(document.has_value(), "fixture envelope parse failed");
		auto base = *document->root().member("base_task_projection")->as_object();
		base.emplace("content_base64", text("forbidden"));
		const auto with_bytes =
			canonical_with_root(input, "base_task_projection", object(std::move(base)));
		auto bytes = decode_provider_worker_v4_ingress(with_bytes);
		require(!bytes && bytes.error().detail == "source-bytes-forbidden",
				"source bytes in base projection were accepted");
	}

	void fault_rejects_bounded_authority_array()
	{
		auto input = make_fixture();
		auto document = parse_json_object(input.envelope);
		require(document.has_value(), "fixture envelope parse failed");
		auto fields = *document->root().as_object();
		auto authority = *fields.at("input_authority").as_object();
		std::vector<json_value> arguments;
		arguments.reserve(4097U);
		for (std::size_t index{}; index < 4097U; ++index)
			arguments.push_back(text("-DTEST=" + std::to_string(index)));
		authority.insert_or_assign("effective_arguments", array(std::move(arguments)));
		auto oversized = decode_provider_worker_v4_ingress(
			canonical_with_root(input, "input_authority", object(std::move(authority))));
		require(!oversized, "oversized explicit argument array was accepted");
	}

	struct protocol_fixture
	{
		cxxlens::sdk::provider::host_transcript_expectation expectation;
		std::vector<std::byte> payload;
		std::vector<std::byte> encoded;
	};

	[[nodiscard]] protocol_fixture make_protocol_fixture()
	{
		std::vector<std::byte> payload;
		payload.reserve(provider_worker_protocol_v2_maximum_chunk_bytes + 17U);
		for (std::size_t index{}; index < provider_worker_protocol_v2_maximum_chunk_bytes + 17U;
			 ++index)
			payload.push_back(static_cast<std::byte>(index % 251U));
		const auto input_digest = cxxlens::sdk::content_digest(payload);
		cxxlens::sdk::provider::open_task_metadata task{
			"task:protocol-v2-input", input_digest, semantic('1'), semantic('2'), content('3')};
		cxxlens::sdk::provider::host_transcript_expectation expectation{
			"provider-manifest-v2", task, {}};
		cxxlens::sdk::provider::host_transcript_request request{
			expectation, {1U << 30U, 256U}, payload};
		auto encoded = cxxlens::sdk::provider::encode_host_transcript(request);
		require(encoded.has_value(), "Protocol 2.0 host transcript encoding failed");
		return {std::move(expectation), std::move(payload), std::move(*encoded)};
	}

	[[nodiscard]] std::vector<cxxlens::sdk::provider::frame>
	decode_protocol_frames(const std::vector<std::byte>& encoded)
	{
		auto frames = cxxlens::sdk::provider::decode_frame_stream(
			encoded, {}, provider_worker_protocol_v2_maximum_frames);
		require(frames.has_value(), "Protocol 2.0 fixture frame decoding failed");
		return std::move(*frames);
	}

	[[nodiscard]] std::vector<std::byte>
	encode_protocol_frames(const std::vector<cxxlens::sdk::provider::frame>& frames)
	{
		std::vector<std::byte> encoded;
		for (const auto& frame : frames)
		{
			auto bytes = cxxlens::sdk::provider::encode_frame(frame);
			require(bytes.has_value(), "Protocol 2.0 fixture frame encoding failed");
			encoded.insert(encoded.end(), bytes->begin(), bytes->end());
		}
		return encoded;
	}

	void append_cbor_head(std::vector<std::byte>& output,
						  const std::uint8_t major,
						  const std::uint64_t value)
	{
		const auto prefix = static_cast<std::uint8_t>(major << 5U);
		if (value < 24U)
			output.push_back(static_cast<std::byte>(prefix | static_cast<std::uint8_t>(value)));
		else if (value <= 0xffU)
		{
			output.push_back(static_cast<std::byte>(prefix | 24U));
			output.push_back(static_cast<std::byte>(value));
		}
		else if (value <= 0xffffU)
		{
			output.push_back(static_cast<std::byte>(prefix | 25U));
			output.push_back(static_cast<std::byte>(value >> 8U));
			output.push_back(static_cast<std::byte>(value));
		}
		else if (value <= 0xffffffffU)
		{
			output.push_back(static_cast<std::byte>(prefix | 26U));
			for (std::size_t shift{24U};; shift -= 8U)
			{
				output.push_back(static_cast<std::byte>(value >> shift));
				if (shift == 0U)
					break;
			}
		}
		else
		{
			output.push_back(static_cast<std::byte>(prefix | 27U));
			for (std::size_t shift{56U};; shift -= 8U)
			{
				output.push_back(static_cast<std::byte>(value >> shift));
				if (shift == 0U)
					break;
			}
		}
	}

	[[nodiscard]] std::vector<std::byte> cbor_text(const std::string_view value)
	{
		std::vector<std::byte> output;
		append_cbor_head(output, 3U, value.size());
		for (const auto byte : value)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	using cbor_test_value = std::variant<std::uint64_t, std::string>;
	struct cbor_test_field
	{
		std::vector<std::byte> key;
		std::vector<std::byte> value;
	};

	[[nodiscard]] std::vector<std::byte>
	input_descriptor_control(const std::string_view task_id,
							 const std::string_view input_digest,
							 const std::uint64_t total_bytes,
							 const std::uint64_t chunk_bytes,
							 const std::uint64_t chunk_count)
	{
		const std::array<std::pair<std::string_view, cbor_test_value>, 6U> fields{
			std::pair{"schema", cbor_test_value{"cxxlens.provider-control.input-descriptor.v1"}},
			std::pair{"task_id", cbor_test_value{std::string{task_id}}},
			std::pair{"input_digest", cbor_test_value{std::string{input_digest}}},
			std::pair{"total_bytes", cbor_test_value{total_bytes}},
			std::pair{"chunk_bytes", cbor_test_value{chunk_bytes}},
			std::pair{"chunk_count", cbor_test_value{chunk_count}}};
		std::vector<cbor_test_field> encoded;
		encoded.reserve(fields.size());
		for (const auto& [key, value] : fields)
		{
			cbor_test_field field{cbor_text(key), {}};
			std::visit(
				[&](const auto& item)
				{
					using item_type = std::remove_cvref_t<decltype(item)>;
					if constexpr (std::same_as<item_type, std::string>)
						field.value = cbor_text(item);
					else
						append_cbor_head(field.value, 0U, item);
				},
				value);
			encoded.push_back(std::move(field));
		}
		std::ranges::sort(encoded,
						  [](const cbor_test_field& left, const cbor_test_field& right)
						  {
							  if (left.key.size() != right.key.size())
								  return left.key.size() < right.key.size();
							  return std::lexicographical_compare(left.key.begin(),
																  left.key.end(),
																  right.key.begin(),
																  right.key.end());
						  });
		std::vector<std::byte> output;
		append_cbor_head(output, 5U, encoded.size());
		for (const auto& field : encoded)
		{
			output.insert(output.end(), field.key.begin(), field.key.end());
			output.insert(output.end(), field.value.begin(), field.value.end());
		}
		return output;
	}

	void positive_decodes_protocol_v2_stdin_to_source_free_launch()
	{
		auto fixture = make_protocol_fixture();
		auto decoded = decode_provider_worker_protocol_v2_input(std::span{fixture.encoded},
																fixture.expectation);
		require(decoded.has_value(), "Protocol 2.0 stdin transcript was rejected");
		require(decoded->provider_manifest == fixture.expectation.provider_manifest &&
					decoded->task == fixture.expectation.task &&
					decoded->output_credit.bytes == (1U << 30U) &&
					decoded->payload == fixture.payload &&
					decoded->protocol_content_digest ==
						fixture.expectation.task.task_input_digest &&
					decoded->stream_id == 1U && decoded->protocol_major == 2U &&
					decoded->protocol_minor == 0U,
				"typed launch envelope lost Protocol content authority");

		const std::string wire(reinterpret_cast<const char*>(fixture.encoded.data()),
							   fixture.encoded.size());
		std::istringstream stdin_wire{wire};
		auto streamed = decode_provider_worker_protocol_v2_input(stdin_wire, fixture.expectation);
		require(streamed.has_value() && streamed->payload == fixture.payload,
				"bounded stdin overload did not reproduce the launch payload");
	}

	void negative_rejects_json_old_version_and_mixed_protocol_input()
	{
		auto fixture = make_protocol_fixture();
		const std::string json{"{}"};
		const auto json_bytes = std::as_bytes(std::span{json.data(), json.size()});
		auto json_result =
			decode_provider_worker_protocol_v2_input(json_bytes, fixture.expectation);
		require(!json_result && json_result.error().detail == "json-input-forbidden",
				"JSON stdin was accepted as a Protocol 2.0 launch");

		auto old_version = fixture.encoded;
		old_version[4U] = std::byte{};
		old_version[5U] = std::byte{1U};
		auto old_result =
			decode_provider_worker_protocol_v2_input(old_version, fixture.expectation);
		require(!old_result, "older frame version was accepted");

		auto mixed = fixture.encoded;
		mixed.push_back(std::byte{'{'});
		auto mixed_result = decode_provider_worker_protocol_v2_input(mixed, fixture.expectation);
		require(!mixed_result, "mixed JSON and Protocol 2.0 stdin was accepted");

		auto inline_frames = decode_protocol_frames(fixture.encoded);
		inline_frames[2U].payload = fixture.payload;
		auto inline_result = decode_provider_worker_protocol_v2_input(
			encode_protocol_frames(inline_frames), fixture.expectation);
		require(!inline_result, "legacy inline task payload transcript was accepted");

		auto semantic_expectation = fixture.expectation;
		semantic_expectation.task.task_input_digest = semantic('9');
		auto semantic_result =
			decode_provider_worker_protocol_v2_input(fixture.encoded, semantic_expectation);
		require(!semantic_result && semantic_result.error().detail == "content-digest-required",
				"task-v4 semantic digest was accepted as Protocol content authority");
	}

	void negative_rejects_reorder_gap_and_oversized_descriptor()
	{
		auto fixture = make_protocol_fixture();
		auto frames = decode_protocol_frames(fixture.encoded);
		require(frames.size() >= 8U, "fixture did not contain two input chunks");
		std::swap(frames[4U], frames[5U]);
		auto reordered = decode_provider_worker_protocol_v2_input(encode_protocol_frames(frames),
																  fixture.expectation);
		require(!reordered, "reordered input chunks were accepted");

		frames = decode_protocol_frames(fixture.encoded);
		++frames[4U].sequence;
		auto gapped = decode_provider_worker_protocol_v2_input(encode_protocol_frames(frames),
															   fixture.expectation);
		require(!gapped, "input sequence gap was accepted");

		frames = decode_protocol_frames(fixture.encoded);
		frames[3U].control =
			input_descriptor_control(fixture.expectation.task.task_id,
									 fixture.expectation.task.task_input_digest,
									 provider_worker_protocol_v2_maximum_input_bytes + 1U,
									 provider_worker_protocol_v2_maximum_chunk_bytes,
									 65U);
		auto oversized = decode_provider_worker_protocol_v2_input(encode_protocol_frames(frames),
																  fixture.expectation);
		require(!oversized, "oversized logical input descriptor was accepted");
	}
} // namespace

int main()
{
	positive_decodes_all_authority();
	negative_rejects_noncanonical_and_missing_fields();
	negative_rejects_identity_and_source_bytes();
	fault_rejects_bounded_authority_array();
	positive_decodes_protocol_v2_stdin_to_source_free_launch();
	negative_rejects_json_old_version_and_mixed_protocol_input();
	negative_rejects_reorder_gap_and_oversized_descriptor();
	return 0;
}
