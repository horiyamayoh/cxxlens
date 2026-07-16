#include "provider_worker.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <map>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/sdk/provider.hpp>

#include "../common/frontend_worker_ipc.hpp"
#include "frontend_job.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using sdk::provider::message_type;

		constexpr std::string_view provider_id = "cxxlens.clang22.reference";
		const sdk::semantic_version provider_version{1U, 0U, 0U};

		[[nodiscard]] sdk::error
		provider_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
		{
			auto input = std::as_bytes(std::span{value});
			return {input.begin(), input.end()};
		}

		[[nodiscard]] sdk::detached_cell symbol_cell(const sdk::scalar_kind kind,
													 std::string parameter,
													 std::string value,
													 const bool optional = false)
		{
			return {{kind, std::move(parameter), optional},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(value)},
					std::nullopt};
		}

		[[nodiscard]] sdk::detached_cell optional_typed(std::string parameter, std::string value)
		{
			auto output = sdk::detached_cell::typed(std::move(parameter), std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] sdk::detached_cell optional_utf8(std::string value)
		{
			auto output = sdk::detached_cell::utf8(std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] sdk::detached_cell optional_bytes(std::vector<std::byte> value)
		{
			auto output = sdk::detached_cell::bytes(std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] std::string payload_projection(const facts::observation_record& observation)
		{
			std::ostringstream output;
			output << static_cast<std::uint16_t>(observation.kind) << '\n';
			for (const auto& [key, value] : observation.payload)
				output << key << '=' << value << '\n';
			if (observation.name)
			{
				output << "name=" << observation.name->display_qualified_name << '\n';
				if (observation.name->semantic_owner)
					output << "owner=" << *observation.name->semantic_owner << '\n';
				if (observation.name->declaration_kind)
					output << "declaration_kind=" << *observation.name->declaration_kind << '\n';
				if (observation.name->signature_structure)
					output << "signature=" << *observation.name->signature_structure << '\n';
			}
			if (observation.type)
				output << "type=" << observation.type->canonical_structure << '\n';
			if (observation.source)
				output << "source=" << observation.source->to_canonical_json() << '\n';
			return output.str();
		}

		[[nodiscard]] std::string semantic_key(const facts::observation_record& observation)
		{
			const auto found = observation.payload.find("semantic_key");
			return found == observation.payload.end()
				? sdk::semantic_digest("frontend.clang22.observation-key.v1",
									   payload_projection(observation))
				: found->second;
		}

		[[nodiscard]] std::string source_id(const source_span& source)
		{
			return sdk::semantic_digest("source-span", source.to_canonical_json());
		}

		[[nodiscard]] sdk::relation_descriptor observation_descriptor(const std::string& name)
		{
			sdk::relation_descriptor descriptor;
			descriptor.id = name + ".v1";
			descriptor.name = name;
			descriptor.version = {1U, 0U, 0U};
			descriptor.semantic_major = 1U;
			descriptor.semantics = name + "/1";
			descriptor.owner_namespace = "cxxlens.provider.clang22";
			const auto prefix = descriptor.id + ".";
			descriptor.columns = {
				{prefix + "observation",
				 "observation",
				 {sdk::scalar_kind::typed_id, "clang22_observation_id", false},
				 true,
				 sdk::column_role::claim_key},
				{prefix + "compile_unit",
				 "compile_unit",
				 {sdk::scalar_kind::typed_id, "compile_unit_id", false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "semantic_key",
				 "semantic_key",
				 {sdk::scalar_kind::bytes, {}, false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "payload_digest",
				 "payload_digest",
				 {sdk::scalar_kind::digest, {}, false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "source",
				 "source",
				 {sdk::scalar_kind::typed_id, "source_span_id", true},
				 false,
				 sdk::column_role::authoritative_payload},
				{prefix + "exact_equivalence",
				 "exact_equivalence",
				 {sdk::scalar_kind::boolean, {}, false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "limitation",
				 "limitation",
				 {sdk::scalar_kind::utf8_string, {}, true},
				 false,
				 sdk::column_role::authoritative_payload},
			};
			descriptor.key_columns = {prefix + "observation"};
			descriptor.merge = sdk::merge_mode::functional_assertion;
			descriptor.conflict_columns = {
				prefix + "compile_unit",
				prefix + "semantic_key",
				prefix + "payload_digest",
				prefix + "source",
				prefix + "exact_equivalence",
				prefix + "limitation",
			};
			descriptor.descriptor_digest =
				sdk::semantic_digest("cxxlens.relation-descriptor.v1", descriptor.canonical_form());
			return descriptor;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		observation_row(const sdk::relation_descriptor& descriptor,
						const facts::observation_record& observation,
						const bool exact,
						const std::string_view limitation)
		{
			sdk::row_builder builder{descriptor};
			const auto key = semantic_key(observation);
			const auto projection = payload_projection(observation);
			const auto prefix = descriptor.id + ".";
			for (auto result : {
					 builder.set(
						 {descriptor.id,
						  prefix + "observation",
						  {sdk::scalar_kind::typed_id, "clang22_observation_id", false}},
						 sdk::detached_cell::typed(
							 "clang22_observation_id",
							 sdk::semantic_digest("frontend.clang22.observation", projection))),
					 builder.set(
						 {descriptor.id,
						  prefix + "compile_unit",
						  {sdk::scalar_kind::typed_id, "compile_unit_id", false}},
						 sdk::detached_cell::typed("compile_unit_id",
												   std::string{observation.compile_unit.value()})),
					 builder.set({descriptor.id,
								  prefix + "semantic_key",
								  {sdk::scalar_kind::bytes, {}, false}},
								 sdk::detached_cell::bytes(bytes(key))),
					 builder.set(
						 {descriptor.id,
						  prefix + "payload_digest",
						  {sdk::scalar_kind::digest, {}, false}},
						 symbol_cell(sdk::scalar_kind::digest,
									 {},
									 sdk::semantic_digest("frontend.clang22.observation-payload.v1",
														  projection))),
					 builder.set({descriptor.id,
								  prefix + "exact_equivalence",
								  {sdk::scalar_kind::boolean, {}, false}},
								 sdk::detached_cell::boolean(exact)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (observation.source)
			{
				auto result =
					builder.set({descriptor.id,
								 prefix + "source",
								 {sdk::scalar_kind::typed_id, "source_span_id", true}},
								optional_typed("source_span_id", source_id(*observation.source)));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			if (!limitation.empty())
			{
				auto result = builder.set({descriptor.id,
										   prefix + "limitation",
										   {sdk::scalar_kind::utf8_string, {}, true}},
										  optional_utf8(std::string{limitation}));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			return std::move(builder).finish();
		}

		[[nodiscard]] std::string limitation_text(const std::vector<std::string>& limitations)
		{
			std::ostringstream output;
			for (std::size_t index = 0U; index < limitations.size(); ++index)
			{
				if (index != 0U)
					output << ',';
				output << limitations[index];
			}
			return output.str();
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		entity_row(const facts::observation_record& observation,
				   const std::string& entity,
				   const std::string& toolchain,
				   const bool exact)
		{
			using relation = cc::relations::entity;
			relation::builder builder;
			const auto projection = payload_projection(observation);
			const auto kind = observation.payload.contains("symbol.kind")
				? observation.payload.at("symbol.kind")
				: "unknown";
			for (auto result : {
					 builder.set<relation::entity_column>(
						 sdk::detached_cell::typed("cc_entity_id", entity)),
					 builder.set<relation::canonicalization>(
						 symbol_cell(sdk::scalar_kind::closed_symbol,
									 "cc.canonicalization-state/1",
									 exact ? "canonical" : "provider_local")),
					 builder.set<relation::kind>(
						 symbol_cell(sdk::scalar_kind::open_symbol, "cc.entity-kind/1", kind)),
					 builder.set<relation::structural_signature_digest>(symbol_cell(
						 sdk::scalar_kind::digest,
						 {},
						 sdk::semantic_digest("cc.entity.structural-signature.v1", projection))),
					 builder.set<relation::toolchain>(
						 optional_typed("toolchain_context_id", toolchain)),
					 builder.set<relation::provider_local_key>(
						 optional_bytes(bytes(semantic_key(observation)))),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (observation.source)
			{
				auto result = builder.set<relation::anchor>(
					optional_typed("source_span_id", source_id(*observation.source)));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			if (observation.name && !observation.name->display_qualified_name.empty())
			{
				auto result = builder.set<relation::qualified_name>(
					optional_utf8(observation.name->display_qualified_name));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		call_site_row(const facts::observation_record& observation,
					  const std::string& call,
					  const std::map<std::string, std::string, std::less<>>& entities,
					  const std::uint64_t ordinal)
		{
			using relation = cc::relations::call_site;
			relation::builder builder;
			const auto caller = observation.payload.find("call.caller");
			const auto kind = observation.payload.contains("call.kind")
				? observation.payload.at("call.kind")
				: "unknown";
			for (auto result : {
					 builder.set<relation::call>(sdk::detached_cell::typed("cc_call_id", call)),
					 builder.set<relation::compile_unit>(sdk::detached_cell::typed(
						 "compile_unit_id", std::string{observation.compile_unit.value()})),
					 builder.set<relation::kind>(
						 symbol_cell(sdk::scalar_kind::open_symbol, "cc.call-kind/1", kind)),
					 builder.set<relation::source>(sdk::detached_cell::typed(
						 "source_span_id", source_id(*observation.source))),
					 builder.set<relation::ordinal>(sdk::detached_cell::unsigned_integer(ordinal)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (caller != observation.payload.end())
			{
				const auto found = entities.find(caller->second);
				if (found != entities.end())
				{
					auto result = builder.set<relation::caller>(
						optional_typed("cc_entity_id", found->second));
					if (!result)
						return sdk::unexpected(std::move(result.error()));
				}
			}
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<sdk::detached_row> direct_target_row(const std::string& call,
																	   const std::string& target)
		{
			using relation = cc::relations::call_direct_target;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::call>(sdk::detached_cell::typed("cc_call_id", call)),
					 builder.set<relation::target>(
						 sdk::detached_cell::typed("cc_entity_id", target)),
					 builder.set<relation::resolution>(symbol_cell(sdk::scalar_kind::open_symbol,
																   "cc.direct-target-resolution/1",
																   "syntactic_direct")),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			return std::move(builder).finish();
		}

		class stream_sink final : public sdk::provider::frame_sink
		{
		  public:
			explicit stream_sink(std::ostream& output) : output_{&output} {}
			sdk::result<void> write(const std::span<const std::byte> data) override
			{
				output_->write(reinterpret_cast<const char*>(data.data()),
							   static_cast<std::streamsize>(data.size()));
				return output_->good()
					? sdk::result<void>{}
					: sdk::unexpected(provider_error("provider.worker-write", "stdout"));
			}

		  private:
			std::ostream* output_;
		};

		[[nodiscard]] std::optional<std::uint64_t> unsigned_field(const std::string_view text,
																  const std::size_t separator)
		{
			if (separator == std::string_view::npos || separator + 1U >= text.size())
				return std::nullopt;
			std::uint64_t output{};
			for (const auto byte : text.substr(separator + 1U))
			{
				if (byte < '0' || byte > '9')
					return std::nullopt;
				const auto digit = static_cast<std::uint64_t>(byte - '0');
				if (output > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
					return std::nullopt;
				output = output * 10U + digit;
			}
			return output;
		}

		class canonical_provider final : public sdk::provider::portable_provider
		{
		  public:
			canonical_provider(frontend::worker_request request, std::string toolchain_digest)
				: request_{std::move(request)}, toolchain_digest_{std::move(toolchain_digest)}
			{
			}

			[[nodiscard]] std::string_view id() const noexcept override
			{
				return provider_id;
			}
			[[nodiscard]] sdk::semantic_version version() const noexcept override
			{
				return provider_version;
			}

			sdk::result<void> run(const sdk::provider::task& task,
								  sdk::provider::context& context) override
			{
				std::vector<std::string> limitations;
				const auto invocation_exact =
					invocation_has_exact_equivalence(request_.task.unit.command(), limitations);
				auto batch = execute(std::move(request_.task));
				if (!batch)
					return sdk::unexpected(provider_error(
						"provider.frontend-failed", task.task_id, batch.error().code.value));
				auto normalized = canonicalize_provider_batch(
					batch.value(), toolchain_digest_, invocation_exact, std::move(limitations));
				if (!normalized)
					return sdk::unexpected(std::move(normalized.error()));

				const auto emit = [&](const sdk::relation_descriptor& descriptor,
									  const std::vector<sdk::detached_row>& rows,
									  const std::string_view group) -> sdk::result<void>
				{
					auto sink = context.relation(descriptor);
					if (auto opened = sink.begin(
							std::string{group}, "clang22-atomic", descriptor.id + "-batch");
						!opened)
						return opened;
					for (const auto& row : rows)
						if (auto pushed = sink.push(row); !pushed)
							return pushed;
					return sink.end();
				};

				for (const auto& [descriptor, rows, group] : {
						 std::tuple{entity_observation_descriptor(),
									&normalized->entity_observations,
									std::string_view{"observation"}},
						 std::tuple{type_observation_descriptor(),
									&normalized->type_observations,
									std::string_view{"observation"}},
						 std::tuple{call_observation_descriptor(),
									&normalized->call_observations,
									std::string_view{"observation"}},
						 std::tuple{cc::relations::entity::descriptor(),
									&normalized->entities,
									std::string_view{"canonical"}},
						 std::tuple{cc::relations::call_site::descriptor(),
									&normalized->call_sites,
									std::string_view{"canonical"}},
						 std::tuple{cc::relations::call_direct_target::descriptor(),
									&normalized->direct_targets,
									std::string_view{"canonical"}},
					 })
					if (auto emitted = emit(descriptor, *rows, group); !emitted)
						return emitted;

				for (auto item : normalized->unresolved)
					context.unresolved().add(std::move(item));
				const auto coverage_state =
					normalized->exact_equivalence ? "covered" : "unresolved";
				const auto reason = limitation_text(normalized->equivalence_limitations);
				for (const auto kind : {
						 "frontend.clang22.observation",
						 "cc.entity",
						 "cc.call-extraction",
					 })
				{
					context.coverage().request(kind, task.task_id);
					if (auto classified = context.coverage().classify(
							{kind, task.task_id, coverage_state, reason});
						!classified)
						return classified;
				}
				context.evidence().add(
					{"provider.clang22.execution",
					 task.task_id,
					 std::string{provider_id},
					 normalized->exact_equivalence ? "exact" : "provider-local"});
				return {};
			}

		  private:
			frontend::worker_request request_;
			std::string toolchain_digest_;
		};
	} // namespace

	sdk::relation_descriptor entity_observation_descriptor()
	{
		return observation_descriptor("frontend.clang22.entity_observation");
	}

	sdk::relation_descriptor type_observation_descriptor()
	{
		return observation_descriptor("frontend.clang22.type_observation");
	}

	sdk::relation_descriptor call_observation_descriptor()
	{
		return observation_descriptor("frontend.clang22.call_observation");
	}

	bool invocation_has_exact_equivalence(const compile_command& command,
										  std::vector<std::string>& limitations)
	{
		static constexpr std::array<std::string_view, 5U> unsupported_semantic_prefixes{
			"-fabi-version",
			"-fno-gnu-unique",
			"-fconcepts-diagnostics-depth",
			"-Wno-psabi",
			"-mabi=",
		};
		for (const auto& argument : command.arguments)
			for (const auto prefix : unsupported_semantic_prefixes)
				if (argument.starts_with(prefix))
					limitations.push_back("ignored-or-gcc-specific-option:" + argument);
		std::ranges::sort(limitations);
		limitations.erase(std::ranges::unique(limitations).begin(), limitations.end());
		return limitations.empty();
	}

	sdk::result<canonicalized_provider_batch>
	canonicalize_provider_batch(const frontend::observation_batch& batch,
								const std::string& toolchain_digest,
								const bool invocation_exact,
								std::vector<std::string> invocation_limitations)
	{
		if (auto valid = batch.validate(); !valid)
			return sdk::unexpected(provider_error(
				"provider.frontend-batch-invalid", "batch", valid.error().code.value));
		if (!toolchain_digest.starts_with("sha256:"))
			return sdk::unexpected(
				provider_error("provider.toolchain-digest-invalid", "toolchain"));

		for (const auto& diagnostic : batch.diagnostics)
			if (diagnostic.severity == frontend::diagnostic_severity::error ||
				diagnostic.severity == frontend::diagnostic_severity::fatal)
				invocation_limitations.push_back("frontend-error-diagnostic:" + diagnostic.id);
		std::ranges::sort(invocation_limitations);
		invocation_limitations.erase(std::ranges::unique(invocation_limitations).begin(),
									 invocation_limitations.end());

		canonicalized_provider_batch output;
		output.exact_equivalence =
			invocation_exact && invocation_limitations.empty() && batch.coverage.failed == 0U;
		output.equivalence_limitations = std::move(invocation_limitations);
		const auto limitation = limitation_text(output.equivalence_limitations);
		const auto toolchain = sdk::semantic_digest("toolchain-context", toolchain_digest);

		std::map<std::string, std::string, std::less<>> entity_ids;
		for (const auto& observation : batch.observations)
			if (observation.kind == fact_kind::symbol)
			{
				const auto legacy_id = observation.payload.find("symbol.id");
				const auto entity =
					sdk::semantic_digest("cc-entity",
										 payload_projection(observation) + "\n" + toolchain + "\n" +
											 std::string{batch.variant.value()});
				if (legacy_id != observation.payload.end())
					entity_ids.emplace(legacy_id->second, entity);
				auto local = observation_row(entity_observation_descriptor(),
											 observation,
											 output.exact_equivalence,
											 limitation);
				auto canonical =
					entity_row(observation, entity, toolchain, output.exact_equivalence);
				if (!local)
					return sdk::unexpected(std::move(local.error()));
				if (!canonical)
					return sdk::unexpected(std::move(canonical.error()));
				output.entity_observations.push_back(std::move(*local));
				output.entities.push_back(std::move(*canonical));
			}

		std::uint64_t call_ordinal{};
		for (const auto& observation : batch.observations)
		{
			if (observation.kind == fact_kind::type)
			{
				auto local = observation_row(type_observation_descriptor(),
											 observation,
											 output.exact_equivalence,
											 limitation);
				if (!local)
					return sdk::unexpected(std::move(local.error()));
				output.type_observations.push_back(std::move(*local));
				continue;
			}
			if (observation.kind != fact_kind::call)
				continue;
			auto local = observation_row(
				call_observation_descriptor(), observation, output.exact_equivalence, limitation);
			if (!local)
				return sdk::unexpected(std::move(local.error()));
			output.call_observations.push_back(std::move(*local));
			if (!observation.source)
			{
				output.unresolved.push_back(
					{"provider.source-unavailable", semantic_key(observation), "cc.call_site"});
				continue;
			}
			const auto call = sdk::semantic_digest(
				"cc-call",
				std::string{batch.unit.value()} + "\n" + std::string{batch.variant.value()} + "\n" +
					source_id(*observation.source) + "\n" + semantic_key(observation) + "\n" +
					std::to_string(call_ordinal));
			auto site = call_site_row(observation, call, entity_ids, call_ordinal++);
			if (!site)
				return sdk::unexpected(std::move(site.error()));
			output.call_sites.push_back(std::move(*site));
			const auto target = observation.payload.find("call.direct_callee");
			const auto mapped = target == observation.payload.end()
				? entity_ids.end()
				: entity_ids.find(target->second);
			if (target == observation.payload.end() || target->second == "unresolved" ||
				mapped == entity_ids.end())
			{
				const auto reason = observation.payload.contains("call.unresolved_reason")
					? observation.payload.at("call.unresolved_reason")
					: "direct-target-unresolved";
				output.unresolved.push_back({"provider.direct-target-unresolved", call, reason});
				continue;
			}
			auto direct = direct_target_row(call, mapped->second);
			if (!direct)
				return sdk::unexpected(std::move(direct.error()));
			output.direct_targets.push_back(std::move(*direct));
		}
		return output;
	}

	int run_provider_worker(const std::span<const std::byte> input, std::ostream& output)
	{
		auto frames = sdk::provider::decode_frame_stream(input);
		if (!frames || frames->size() != 5U || frames->at(0U).type != message_type::hello_ack ||
			frames->at(2U).type != message_type::open_task ||
			frames->at(3U).type != message_type::credit)
			return EXIT_FAILURE;
		auto hello = sdk::provider::decode_control_text(frames->at(0U).control);
		auto task_control = sdk::provider::decode_control_text(frames->at(2U).control);
		auto credit = sdk::provider::decode_control_text(frames->at(3U).control);
		if (!hello || !task_control || !credit)
			return EXIT_FAILURE;
		const auto first_credit_separator = credit->find('|');
		auto byte_credit = unsigned_field(*credit, std::string_view::npos);
		if (first_credit_separator != std::string::npos)
		{
			std::uint64_t parsed{};
			for (const auto byte : std::string_view{*credit}.substr(0U, first_credit_separator))
			{
				if (byte < '0' || byte > '9')
					return EXIT_FAILURE;
				parsed = parsed * 10U + static_cast<std::uint64_t>(byte - '0');
			}
			byte_credit = parsed;
		}
		const auto frame_credit = unsigned_field(*credit, first_credit_separator);
		if (!byte_credit || !frame_credit || *byte_credit == 0U || *frame_credit == 0U)
			return EXIT_FAILURE;

		const auto hello_parts = std::string_view{*hello};
		if (!hello_parts.starts_with(std::string{provider_id} + "|1.0.0|"))
			return EXIT_FAILURE;
		if (std::ranges::count(hello_parts, '|') != 3)
			return EXIT_FAILURE;

		stream_sink sink{output};
		sdk::provider::protocol_writer writer{sink};
		writer.grant_credit({*byte_credit, *frame_credit});
		if (!writer.send(message_type::hello, frames->at(0U).control))
			return EXIT_FAILURE;

		const std::string payload{reinterpret_cast<const char*>(frames->at(2U).payload.data()),
								  frames->at(2U).payload.size()};
		auto request = frontend::decode_worker_request(payload);
		if (!request)
		{
			auto failed = bytes("provider.frontend-request-invalid|payload");
			std::vector<std::byte> control{static_cast<std::byte>(0x78U),
										   static_cast<std::byte>(failed.size())};
			control.insert(control.end(), failed.begin(), failed.end());
			(void)writer.send(message_type::task_failed, control);
			return EXIT_SUCCESS;
		}
		const auto control_parts = std::string_view{*task_control};
		const auto separator = control_parts.find('|');
		if (separator == std::string_view::npos)
			return EXIT_FAILURE;
		const std::string task_id{control_parts.substr(0U, separator)};
		sdk::provider::task task{
			task_id,
			{std::string{request.value().task.unit.id().value()},
			 sdk::semantic_digest("cxxlens.provider-project.v1",
								  request.value().task.unit.id().value()),
			 "provider-input",
			 {std::string{request.value().task.unit.id().value()}}},
			{entity_observation_descriptor(),
			 type_observation_descriptor(),
			 call_observation_descriptor(),
			 cc::relations::entity::descriptor(),
			 cc::relations::call_site::descriptor(),
			 cc::relations::call_direct_target::descriptor()},
			"all",
			"cc.clang22-canonical-1",
		};
		const auto toolchain_separator = control_parts.rfind('|');
		const auto environment_separator = toolchain_separator == std::string_view::npos
			? std::string_view::npos
			: control_parts.rfind('|', toolchain_separator - 1U);
		const std::string toolchain_digest{
			environment_separator == std::string_view::npos
				? std::string_view{}
				: control_parts.substr(environment_separator + 1U,
									   toolchain_separator - environment_separator - 1U)};
		canonical_provider provider{std::move(request.value()), toolchain_digest};
		(void)sdk::provider::run_worker(provider, task, writer);
		return EXIT_SUCCESS;
	}
} // namespace cxxlens::detail::clang22
