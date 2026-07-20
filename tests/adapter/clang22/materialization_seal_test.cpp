#include "llvm/clang22/materialization_seal.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/source_file.hpp>

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;
	using namespace cxxlens::detail::clang22::materialization;

	constexpr std::string_view fixture_semantic_contract_digest =
		"sha256:1111111111111111111111111111111111111111111111111111111111111111";

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	class fixture_row
	{
	  public:
		explicit fixture_row(const sdk::relation_descriptor& descriptor)
			: descriptor_{descriptor}, row_{descriptor.id, {}}
		{
		}

		fixture_row& string(const std::string_view name, std::string value)
		{
			auto column = descriptor_.column(name);
			require(column.has_value(), "fixture column is absent: " + std::string{name});
			row_.cells.emplace(column->id,
							   sdk::detached_cell{column->type,
												  sdk::cell_state::present,
												  sdk::scalar_value{std::move(value)},
												  std::nullopt});
			return *this;
		}

		fixture_row& unsigned_integer(const std::string_view name, const std::uint64_t value)
		{
			auto column = descriptor_.column(name);
			require(column.has_value(), "fixture uint column is absent: " + std::string{name});
			auto cell = sdk::detached_cell::unsigned_integer(value);
			require(cell.type == column->type, "fixture uint column type differs");
			row_.cells.emplace(column->id, std::move(cell));
			return *this;
		}

		fixture_row& boolean(const std::string_view name, const bool value)
		{
			auto column = descriptor_.column(name);
			require(column.has_value(), "fixture bool column is absent: " + std::string{name});
			auto cell = sdk::detached_cell::boolean(value);
			require(cell.type == column->type, "fixture bool column type differs");
			row_.cells.emplace(column->id, std::move(cell));
			return *this;
		}

		fixture_row& absent(const std::string_view name)
		{
			auto column = descriptor_.column(name);
			require(column.has_value(), "fixture optional column is absent: " + std::string{name});
			row_.cells.emplace(column->id, sdk::detached_cell::absent(column->type));
			return *this;
		}

		[[nodiscard]] sdk::detached_row finish_identity(const std::string_view result_name)
		{
			auto result = descriptor_.column(result_name);
			require(result.has_value(), "fixture result column is absent");
			row_.cells.emplace(result->id,
							   sdk::detached_cell{result->type,
												  sdk::cell_state::present,
												  sdk::scalar_value{std::string{"pending"}},
												  std::nullopt});
			auto identity = sdk::derive_domain_identity(descriptor_, row_);
			require(identity.has_value(), "fixture identity derivation failed");
			row_.cells.at(result->id).value = sdk::scalar_value{std::move(*identity)};
			require(sdk::validate_row(descriptor_, row_).has_value() &&
						sdk::validate_domain_identity(descriptor_, row_).has_value(),
					"fixture identity row is invalid");
			return std::move(row_);
		}

		[[nodiscard]] sdk::detached_row finish()
		{
			require(sdk::validate_row(descriptor_, row_).has_value(),
					"fixture resultless row is invalid");
			return std::move(row_);
		}

	  private:
		const sdk::relation_descriptor& descriptor_;
		sdk::detached_row row_;
	};

	[[nodiscard]] std::string result_string(const sdk::detached_row& row,
											const std::string_view column_id)
	{
		const auto found = row.cells.find(column_id);
		require(found != row.cells.end() && found->second.value.has_value(),
				"fixture result cell is absent");
		const auto* value = std::get_if<std::string>(&*found->second.value);
		require(value != nullptr, "fixture result cell has the wrong type");
		return *value;
	}

	[[nodiscard]] std::vector<sdk::relation_descriptor> exact_outputs()
	{
		return {
			cc::relations::call_direct_target::descriptor(),
			cc::relations::call_site::descriptor(),
			cc::relations::entity::descriptor(),
			call_observation_v2_descriptor(),
			entity_observation_v2_descriptor(),
			type_observation_v2_descriptor(),
		};
	}

	[[nodiscard]] clang22_task_input task_input()
	{
		const std::string source{"int target(); int main(){ return target(); }"};
		const std::vector<std::string> arguments{"clang++", "-std=c++23"};
		auto invocation = sdk::canonical_binary(sdk::canonical_value::from_tuple({
			sdk::canonical_value::from_string("cxxlens.clang22.effective-invocation.v1"),
			sdk::canonical_value::from_string("project://workspace"),
			sdk::canonical_value::from_tuple({sdk::canonical_value::from_string("clang++"),
											  sdk::canonical_value::from_string("-std=c++23")}),
		}));
		require(invocation.has_value(), "fixture invocation projection failed");
		const std::string invocation_bytes{reinterpret_cast<const char*>(invocation->data()),
										   invocation->size()};
		auto invocation_digest =
			sdk::semantic_digest("cxxlens.clang22.effective-invocation.v1", invocation_bytes);
		require(invocation_digest.has_value(), "fixture invocation digest failed");
		const auto source_digest = sdk::content_digest(std::as_bytes(std::span{source}));
		const std::string environment_digest =
			"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
		auto catalog = sdk::project_catalog::make(
			"project://workspace",
			environment_digest,
			{{"catalog-unit:alpha", *invocation_digest, source_digest, environment_digest},
			 {"catalog-unit:beta",
			  "sha256:abababababababababababababababababababababababababababababababab",
			  "sha256:bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbc",
			  environment_digest}});
		require(catalog.has_value(),
				catalog ? ""
						: "fixture project catalog failed: " + catalog.error().code + "/" +
						catalog.error().field + "/" + catalog.error().detail);

		clang22_task_input output;
		output.project_catalog = std::move(*catalog);
		output.selected_catalog_compile_unit = "catalog-unit:alpha";
		output.toolchain_digest =
			"semantic-v2:sha256:"
			"7f3c5aa5ac281a5c00cf543ae51a36941a043f79b08988d4b928dcd9befb8c3c";
		output.toolchain = {
			"toolchain-family:clang",
			"toolchain-version:22.0.0",
			"target:x86_64-linux-gnu",
			"sha256:2222222222222222222222222222222222222222222222222222222222222222",
			std::nullopt,
			"sha256:3333333333333333333333333333333333333333333333333333333333333333",
			"sha256:4444444444444444444444444444444444444444444444444444444444444444",
		};
		output.variant_authority = {
			"language:cxx",
			"standard:cxx23",
			"target:x86_64-linux-gnu",
			"sha256:5555555555555555555555555555555555555555555555555555555555555555",
			"sha256:6666666666666666666666666666666666666666666666666666666666666666",
			"sha256:7777777777777777777777777777777777777777777777777777777777777777",
		};
		output.normalized_invocation_digest = *invocation_digest;
		output.environment_digest = environment_digest;
		output.language = "language:cxx";
		output.working_directory = "project://workspace";
		output.condition_universe = "condition-universe:alpha";
		output.condition = "condition:alpha";
		output.interpretation = "cc.clang22-canonical-1";
		output.logical_path = "project://input.cpp";
		output.source_content_digest = source_digest;
		output.source_content_base64 =
			"aW50IHRhcmdldCgpOyBpbnQgbWFpbigpeyByZXR1cm4gdGFyZ2V0KCk7IH0=";
		output.source_size_bytes = source.size();
		output.source_encoding = "utf8";
		output.source_read_only = true;
		output.source = source;
		output.arguments = arguments;
		for (const auto& descriptor : exact_outputs())
			output.requested_descriptors.push_back(descriptor.id);
		output.dependency_groups = {"canonical", "observation"};
		output.sandbox = {
			"enforced",
			"sha256:8888888888888888888888888888888888888888888888888888888888888888",
		};

		const std::array file_projection{
			sdk::canonical_value::from_string("project"),
			sdk::canonical_value::from_string("input.cpp"),
			sdk::canonical_value::from_string("cxxlens.logical-path.v1"),
		};
		auto file = sdk::canonical_identity_digest("file", file_projection);
		require(file.has_value(), "fixture file identity failed");
		output.file = std::move(*file);
		const std::array line_index_projection{
			sdk::canonical_value::from_string("cxxlens.byte-line-index.v1"),
			sdk::canonical_value::from_string(output.source_content_digest),
			sdk::canonical_value::from_integer(static_cast<std::int64_t>(source.size())),
			sdk::canonical_value::from_tuple({sdk::canonical_value::from_integer(0)}),
		};
		auto line_index = sdk::canonical_identity_digest("line-index", line_index_projection);
		require(line_index.has_value(), "fixture line-index identity failed");
		output.line_index = std::move(*line_index);

		const auto& project = build::relations::project::descriptor();
		auto project_row =
			fixture_row{project}
				.string("catalog", output.project_catalog.catalog_id)
				.string("catalog_digest", output.project_catalog.catalog_digest)
				.string("logical_root", output.project_catalog.logical_root)
				.string("environment_digest", output.project_catalog.environment_digest)
				.finish_identity("project");
		output.project = result_string(project_row, "build.project.v1.project");

		const auto& toolchain = build::relations::toolchain_context::descriptor();
		auto toolchain_row =
			fixture_row{toolchain}
				.string("family", output.toolchain.family)
				.string("exact_version", output.toolchain.exact_version)
				.string("target_triple", output.toolchain.target_triple)
				.string("builtin_headers_digest", output.toolchain.builtin_headers_digest)
				.absent("sysroot")
				.string("abi_digest", output.toolchain.abi_digest)
				.string("plugin_spec_digest", output.toolchain.plugin_spec_digest)
				.finish_identity("toolchain");
		output.toolchain_context =
			result_string(toolchain_row, "build.toolchain_context.v1.toolchain");

		const auto& variant = build::relations::variant::descriptor();
		auto variant_row =
			fixture_row{variant}
				.string("project", output.project)
				.string("toolchain", output.toolchain_context)
				.string("language", output.variant_authority.language)
				.string("language_standard", output.variant_authority.language_standard)
				.string("target_triple", output.variant_authority.target_triple)
				.string("predefined_macros_digest",
						output.variant_authority.predefined_macros_digest)
				.string("include_search_digest", output.variant_authority.include_search_digest)
				.string("semantic_flags_digest", output.variant_authority.semantic_flags_digest)
				.finish_identity("variant");
		output.variant = result_string(variant_row, "build.variant.v1.variant");

		const auto& source_descriptor = source::relations::file::descriptor();
		auto source_row = fixture_row{source_descriptor}
							  .string("file", output.file)
							  .string("project", output.project)
							  .string("logical_path", output.logical_path)
							  .string("content", output.source_content_digest)
							  .unsigned_integer("size", output.source_size_bytes)
							  .string("encoding", output.source_encoding)
							  .string("line_index", output.line_index)
							  .boolean("read_only", output.source_read_only)
							  .finish_identity("snapshot");
		output.source_snapshot = result_string(source_row, "source.file.v1.snapshot");

		const auto& compile_unit = build::relations::compile_unit::descriptor();
		auto compile_unit_row =
			fixture_row{compile_unit}
				.string("project", output.project)
				.string("main_source", output.source_snapshot)
				.string("variant", output.variant)
				.string("toolchain", output.toolchain_context)
				.string("effective_invocation_digest", output.normalized_invocation_digest)
				.string("language", output.language)
				.string("working_directory", output.working_directory)
				.finish_identity("compile_unit");
		output.compile_unit = result_string(compile_unit_row, "build.compile_unit.v1.compile_unit");
		require(output.validate().has_value(), "fixture task.v3 input is invalid");
		return output;
	}

	enum class fixture_mode : std::uint8_t
	{
		valid,
		missing_batch,
		wrong_batch_order,
		wrong_dependency_group,
		direct_target_missing_call,
		observation_compile_unit_drift,
		span_bundle_conflict,
		equivalence_coupling,
	};

	[[nodiscard]] sdk::detached_row call_site_row(const clang22_task_input& input,
												  const std::string& span_id)
	{
		const auto& descriptor = cc::relations::call_site::descriptor();
		return fixture_row{descriptor}
			.string("compile_unit", input.compile_unit)
			.absent("caller")
			.string("kind", "direct_function")
			.string("source", span_id)
			.absent("receiver_static_type")
			.unsigned_integer("ordinal", 0U)
			.finish_identity("call");
	}

	void rebind_observation_identity(sdk::detached_row& row,
									 const sdk::relation_descriptor& descriptor)
	{
		auto identity = sdk::derive_domain_identity(descriptor, row);
		require(identity.has_value(), "observation fixture rebind failed");
		auto& result = row.cells.at(descriptor.id + ".observation");
		result.value = sdk::scalar_value{std::move(*identity)};
		require(sdk::validate_domain_identity(descriptor, row).has_value(),
				"observation fixture rebind is invalid");
	}

	[[nodiscard]] std::array<std::vector<sdk::detached_row>, 6U>
	fixture_rows(const clang22_task_input& input, const fixture_mode mode)
	{
		observation_v2_task_authority authority{
			.final_relation_compile_unit_id = input.compile_unit,
			.source_snapshot_id = input.source_snapshot,
			.source_file_id = input.file,
			.source_size_bytes = input.source_size_bytes,
		};
		auto span_id =
			sdk::source_span_identity(input.source_snapshot, input.file, 4U, 10U, "spelling");
		require(span_id.has_value(), "fixture source span identity failed");
		observation_v2_primary_span shared_span{
			*span_id, input.source_snapshot, input.file, 4U, 10U, "spelling", true};

		native_observation_v2 call{
			.kind = observation_v2_kind::call,
			.final_relation_compile_unit_id = input.compile_unit,
			.semantic_key = "call:target",
			.payload = {{"kind", "direct_function"}},
			.primary_span = shared_span,
			.origin_chain = {},
			.exact_equivalence = true,
			.limitation = std::nullopt,
		};
		auto entity_authority = authority;
		if (mode == fixture_mode::observation_compile_unit_drift)
			entity_authority.final_relation_compile_unit_id = "compile-unit:other";
		auto entity_span = shared_span;
		if (mode == fixture_mode::span_bundle_conflict)
			entity_span.read_only = false;
		native_observation_v2 entity{
			.kind = observation_v2_kind::entity,
			.final_relation_compile_unit_id = entity_authority.final_relation_compile_unit_id,
			.semantic_key = "entity:target",
			.payload = {{"kind", "function"}},
			.primary_span = entity_span,
			.origin_chain = {},
			.exact_equivalence = true,
			.limitation = std::nullopt,
		};
		native_observation_v2 type{
			.kind = observation_v2_kind::type,
			.final_relation_compile_unit_id = input.compile_unit,
			.semantic_key = "type:int",
			.payload = {{"canonical", "int"}},
			.primary_span = std::nullopt,
			.origin_chain = {},
			.exact_equivalence = false,
			.limitation = "fixture approximation",
		};

		auto call_row = make_observation_v2_row(call, authority);
		auto entity_row = make_observation_v2_row(entity, entity_authority);
		auto type_row = make_observation_v2_row(type, authority);
		require(call_row && entity_row && type_row, "observation fixture construction failed");
		if (mode == fixture_mode::equivalence_coupling)
		{
			const auto& descriptor = type_observation_v2_descriptor();
			auto exact = descriptor.column("exact_equivalence");
			require(exact.has_value(), "type observation exactness column is absent");
			type_row->cells.at(exact->id) = sdk::detached_cell::boolean(true);
			rebind_observation_identity(*type_row, descriptor);
		}

		auto call_site = call_site_row(input, *span_id);
		auto call_id = result_string(call_site, "cc.call_site.v1.call");
		if (mode == fixture_mode::direct_target_missing_call)
			call_id = "cc-call:missing";
		const auto& direct_target = cc::relations::call_direct_target::descriptor();
		auto direct_target_row = fixture_row{direct_target}
									 .string("call", std::move(call_id))
									 .string("target", "cc-entity:fixture-target")
									 .string("resolution", "syntactic_direct")
									 .finish();

		std::array<std::vector<sdk::detached_row>, 6U> rows;
		rows[0U].push_back(std::move(direct_target_row));
		rows[1U].push_back(std::move(call_site));
		rows[3U].push_back(std::move(*call_row));
		rows[4U].push_back(std::move(*entity_row));
		rows[5U].push_back(std::move(*type_row));
		return rows;
	}

	class transcript_sink final : public sdk::provider::frame_sink
	{
	  public:
		sdk::result<void> write(const std::span<const std::byte> bytes) override
		{
			transcript.insert(transcript.end(), bytes.begin(), bytes.end());
			return {};
		}

		std::vector<std::byte> transcript;
	};

	class fixture_provider final : public sdk::provider::portable_provider
	{
	  public:
		fixture_provider(std::array<std::vector<sdk::detached_row>, 6U> rows, fixture_mode mode)
			: rows_{std::move(rows)}, mode_{mode}
		{
		}

		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "cxxlens.clang22.reference";
		}

		[[nodiscard]] sdk::semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}

		[[nodiscard]] std::string_view semantic_contract_digest() const noexcept override
		{
			return fixture_semantic_contract_digest;
		}

		sdk::result<void> run(const sdk::provider::task& task,
							  sdk::provider::context& context) override
		{
			const auto descriptors = exact_outputs();
			std::array<std::size_t, 6U> order{0U, 1U, 2U, 3U, 4U, 5U};
			if (mode_ == fixture_mode::wrong_batch_order)
				std::swap(order[0U], order[1U]);
			for (const auto index : order)
			{
				if (mode_ == fixture_mode::missing_batch && index == 5U)
					continue;
				const auto& descriptor = descriptors[index];
				auto output = context.relation(descriptor);
				const std::string dependency_group =
					mode_ == fixture_mode::wrong_dependency_group && index == 4U
					? "canonical"
					: (index < 3U ? "canonical" : "observation");
				if (auto begun =
						output.begin(dependency_group, "clang22-atomic", descriptor.id + "-batch");
					!begun)
					return begun;
				for (const auto& row : rows_[index])
					if (auto pushed = output.push(row); !pushed)
						return pushed;
				if (auto ended = output.end(); !ended)
					return ended;
			}
			context.coverage().request("task", task.task_id);
			return context.coverage().classify({"task", task.task_id, "covered", {}});
		}

	  private:
		std::array<std::vector<sdk::detached_row>, 6U> rows_;
		fixture_mode mode_;
	};

	struct sealed_fixture
	{
		validated_task_request request;
		sdk::provider::detail::sealed_provider_transcript generic_seal;
	};

	[[nodiscard]] sealed_fixture generic_seal(const fixture_mode mode)
	{
		auto input = task_input();
		auto outputs = exact_outputs();
		auto task = reconstruct_provider_task(
			input, outputs, std::string{fixture_semantic_contract_digest});
		require(task.has_value(), "portable task reconstruction failed");
		auto payload = encode_task_input(input);
		require(payload.has_value(), "task.v3 payload encoding failed");
		validated_task_request request{
			.worker_input = input,
			.provider_task_id = task->task_id,
			.provider_execution_id = "provider-execution:fixture",
			.task_input_digest = sdk::content_digest(*payload),
			.sandbox = {sdk::provider::sandbox_assurance::enforced, input.sandbox.policy_digest},
			.worker_payload = *payload,
		};

		fixture_provider provider{fixture_rows(input, mode), mode};
		transcript_sink sink;
		sdk::provider::protocol_writer writer{sink};
		const sdk::provider::protocol_credit credit{64U * 1024U * 1024U, 65536U};
		writer.grant_credit(credit);
		sdk::provider::execution_context execution;
		execution.budget = input.budget;
		require(sdk::provider::run_worker(provider, *task, writer, execution).has_value(),
				"fixture provider run failed");
		auto frames = sdk::provider::decode_frame_stream(sink.transcript);
		require(frames.has_value(), "fixture transcript decode failed");
		const sdk::provider::detail::transcript_validation_request validation_request{
			task->task_id,
			std::string{provider.id()},
			provider.version(),
			nullptr,
			task->outputs,
			credit,
			&execution.budget,
			false,
		};
		auto validated = sdk::provider::detail::validate_provider_transcript(
			validation_request, *frames, sdk::provider::protocol_limits{});
		const auto validation_failure = [&]
		{
			if (!validated)
				return validated.error().code + "/" + validated.error().field + "/" +
					validated.error().detail;
			if (validated->sealing_error())
				return validated->reason + "/" + validated->sealing_error()->code + "/" +
					validated->sealing_error()->field + "/" + validated->sealing_error()->detail;
			return validated->reason;
		}();
		require(validated &&
					validated->kind == sdk::provider::detail::transcript_terminal_kind::complete &&
					validated->sealed().has_value() && !validated->sealing_error(),
				"generic transcript seal failed: " + validation_failure);
		auto seal = std::move(*validated).take_sealed();
		require(seal.has_value(), "generic transcript seal was not movable");
		return {std::move(request), std::move(*seal)};
	}

	void positive_seal()
	{
		auto fixture = generic_seal(fixture_mode::valid);
		auto sealed =
			validate_and_seal_materialization(fixture.request, std::move(fixture.generic_seal));
		require(sealed.has_value(),
				sealed ? ""
					   : "higher seal failed: " + sealed.error().code + "/" + sealed.error().field +
						"/" + sealed.error().detail);
		require(sealed->provider_task_id() == fixture.request.provider_task_id &&
					sealed->task_input_digest() == fixture.request.task_input_digest &&
					sealed->provider_execution_id() == fixture.request.provider_execution_id &&
					sealed->selected_catalog_compile_unit_id() ==
						fixture.request.worker_input.selected_catalog_compile_unit &&
					sealed->final_relation_compile_unit_id() ==
						fixture.request.worker_input.compile_unit,
				"higher seal lost the exact task binding");
		const std::array expected_base{
			std::string_view{"build.project.v1"},
			std::string_view{"build.toolchain_context.v1"},
			std::string_view{"build.variant.v1"},
			std::string_view{"source.file.v1"},
			std::string_view{"build.compile_unit.v1"},
		};
		require(sealed->provider_seal().batches().size() == 6U &&
					sealed->provider_seal().batches()[0U].rows().size() == 1U &&
					sealed->base_claim_rows().size() == expected_base.size() &&
					sealed->source_span_claim_rows().size() == 1U &&
					sealed->observation_rows().size() == 3U,
				"higher seal census differs from the exact contract");
		require(!cc::relations::call_direct_target::descriptor().domain_identity.result_column &&
					sdk::validate_row(cc::relations::call_direct_target::descriptor(),
									  sealed->provider_seal().batches()[0U].rows().front())
						.has_value(),
				"resultless direct-target row did not survive generic and higher sealing");
		for (std::size_t index{}; index < expected_base.size(); ++index)
			require(sealed->base_claim_rows()[index].descriptor_id == expected_base[index],
					"base claims are not in dependency order");
		require(sealed->source_span_claim_rows().front().descriptor_id == "source.span.v1" &&
					!sealed->observation_rows().back().observation.exact_equivalence &&
					sealed->observation_rows().back().observation.limitation ==
						"fixture approximation",
				"span dedup or typed approximation retention failed");
	}

	void negative_seals()
	{
		for (const auto [mode, expected_code] : {
				 std::pair{fixture_mode::missing_batch,
						   std::string_view{"materialization.group-incomplete"}},
				 std::pair{fixture_mode::wrong_batch_order,
						   std::string_view{"materialization.group-incomplete"}},
				 std::pair{fixture_mode::wrong_dependency_group,
						   std::string_view{"materialization.group-incomplete"}},
				 std::pair{fixture_mode::direct_target_missing_call,
						   std::string_view{"materialization.claim-invalid"}},
				 std::pair{fixture_mode::observation_compile_unit_drift,
						   std::string_view{"materialization.task-binding-mismatch"}},
				 std::pair{fixture_mode::span_bundle_conflict,
						   std::string_view{"materialization.span-invalid"}},
				 std::pair{fixture_mode::equivalence_coupling,
						   std::string_view{"materialization.claim-invalid"}},
			 })
		{
			auto fixture = generic_seal(mode);
			auto sealed =
				validate_and_seal_materialization(fixture.request, std::move(fixture.generic_seal));
			require(!sealed && sealed.error().code == expected_code,
					"higher seal negative mode " + std::to_string(static_cast<unsigned>(mode)) +
						" returned " +
						(sealed ? std::string{"success"}
								: sealed.error().code + "/" + sealed.error().field + "/" +
								 sealed.error().detail));
		}

		auto fixture = generic_seal(fixture_mode::valid);
		fixture.request.provider_task_id = "task:other";
		auto task_drift =
			validate_and_seal_materialization(fixture.request, std::move(fixture.generic_seal));
		require(!task_drift && task_drift.error().code == "materialization.task-binding-mismatch",
				"generic-seal task drift was accepted");
	}
} // namespace

int main()
{
	static_assert(!std::is_copy_constructible_v<sealed_materialization_result>);
	static_assert(!std::is_copy_assignable_v<sealed_materialization_result>);
	static_assert(std::is_move_constructible_v<sealed_materialization_result>);
	static_assert(std::is_move_assignable_v<sealed_materialization_result>);
	positive_seal();
	negative_seals();
}
