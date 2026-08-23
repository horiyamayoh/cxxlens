#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/provider_task_v4.hpp"
#include "llvm/clang22/provider_worker_ingress.hpp"

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
} // namespace

int main()
{
	positive_decodes_all_authority();
	negative_rejects_noncanonical_and_missing_fields();
	negative_rejects_identity_and_source_bytes();
	fault_rejects_bounded_authority_array();
	return 0;
}
