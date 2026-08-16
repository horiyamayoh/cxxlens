#include "llvm/clang22/materialization_claims.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>

#include "llvm/clang22/materialization_bounded_claim_source.hpp"
#include "llvm/clang22/materialization_claim_stream.hpp"
#include "llvm/clang22/materialization_incremental_coordinator.hpp"
#include "llvm/clang22/materialization_incremental_ingress.hpp"
#include "llvm/clang22/materialization_incremental_receipt.hpp"
#include "llvm/clang22/materialization_partition_event_stream.hpp"
#include "llvm/clang22/materialization_pipeline.hpp"
#include "llvm/clang22/materialization_request_identity.hpp"
#include "llvm/clang22/materialization_request_v2_1.hpp"
#include "sdk/provider_runtime_internal.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;
	using namespace cxxlens::detail::clang22::materialization;
	namespace incremental = cxxlens::sdk::incremental;

	constexpr std::string_view worker_semantics =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::string failure(const sdk::error& value)
	{
		return value.code + "/" + value.field + "/" + value.detail;
	}

	[[nodiscard]] std::string stored_claim_ref(const sdk::claim& value)
	{
		auto singleton =
			sdk::claim_batch_content_digest(std::span<const sdk::claim>{&value, 1U}, {}, {}, {});
		require(singleton.has_value(), "committed claim singleton digest failed");
		const std::array fields{sdk::canonical_value::from_string("stored_final"),
								sdk::canonical_value::from_string(*singleton)};
		auto reference = sdk::canonical_identity_digest("materialization-claim-envelope", fields);
		require(reference.has_value(), "committed claim reference derivation failed");
		return std::move(*reference);
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
			require(column.has_value(), "fixture uint column is absent");
			auto cell = sdk::detached_cell::unsigned_integer(value);
			require(cell.type == column->type, "fixture uint column type differs");
			row_.cells.emplace(column->id, std::move(cell));
			return *this;
		}

		fixture_row& absent(const std::string_view name)
		{
			auto column = descriptor_.column(name);
			require(column.has_value(), "fixture optional column is absent");
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

	[[nodiscard]] std::string row_string(const sdk::detached_row& row,
										 const std::string_view column_id)
	{
		const auto found = row.cells.find(column_id);
		require(found != row.cells.end() && found->second.value.has_value(),
				"fixture result cell is absent");
		const auto* value = std::get_if<std::string>(&*found->second.value);
		require(value != nullptr, "fixture result cell has the wrong type");
		return *value;
	}

	[[nodiscard]] std::array<std::vector<sdk::detached_row>, 6U>
	fixture_rows(const clang22_task_input& input, const bool empty, const bool missing_call)
	{
		std::array<std::vector<sdk::detached_row>, 6U> rows;
		if (empty)
			return rows;

		auto span_id =
			sdk::source_span_identity(input.source_snapshot, input.file, 0U, 1U, "spelling");
		require(span_id.has_value(), "fixture source span identity failed");
		observation_v2_primary_span span{
			*span_id, input.source_snapshot, input.file, 0U, 1U, "spelling", true};
		observation_v2_task_authority task_authority{
			input.compile_unit, input.source_snapshot, input.file, input.source_size_bytes};

		const auto signature =
			sdk::semantic_digest("cc.entity.structural-signature.v1", "fixture-target");
		require(signature.has_value(), "fixture entity signature failed");
		const auto& entity_descriptor = cc::relations::entity::descriptor();
		auto entity = fixture_row{entity_descriptor}
						  .string("canonicalization", "canonicalized")
						  .string("kind", "function")
						  .absent("semantic_owner")
						  .string("structural_signature_digest", *signature)
						  .absent("anchor")
						  .string("toolchain", input.toolchain_context)
						  .absent("provider_local_key")
						  .absent("qualified_name")
						  .finish_identity("entity");
		const auto entity_id = row_string(entity, "cc.entity.v1.entity");

		const auto& call_site_descriptor = cc::relations::call_site::descriptor();
		auto call_site = fixture_row{call_site_descriptor}
							 .string("compile_unit", input.compile_unit)
							 .absent("caller")
							 .string("kind", "direct_function")
							 .string("source", *span_id)
							 .absent("receiver_static_type")
							 .unsigned_integer("ordinal", 0U)
							 .finish_identity("call");
		auto call_id = row_string(call_site, "cc.call_site.v1.call");
		if (missing_call)
			call_id = "cc-call:missing";
		const auto& target_descriptor = cc::relations::call_direct_target::descriptor();
		auto target = fixture_row{target_descriptor}
						  .string("call", std::move(call_id))
						  .string("target", entity_id)
						  .string("resolution", "syntactic_direct")
						  .finish();

		const auto observation = [&](const observation_v2_kind kind,
									 const std::string_view semantic_key,
									 const std::string_view payload_key,
									 const std::string_view payload_value,
									 const bool source)
		{
			native_observation_v2 value{
				.kind = kind,
				.final_relation_compile_unit_id = input.compile_unit,
				.semantic_key = std::string{semantic_key},
				.payload = {{std::string{payload_key}, std::string{payload_value}}},
				.primary_span = source ? std::optional{span} : std::nullopt,
				.origin_chain = {},
				.exact_equivalence = true,
				.limitation = std::nullopt,
			};
			auto row = make_observation_v2_row(value, task_authority);
			require(row.has_value(),
					"fixture observation row failed: " +
						(row ? std::string{} : failure(row.error())));
			return std::move(*row);
		};

		rows[0U].push_back(std::move(target));
		rows[1U].push_back(std::move(call_site));
		rows[2U].push_back(std::move(entity));
		rows[3U].push_back(observation(
			observation_v2_kind::call, "call:fixture", "kind", "direct_function", true));
		rows[4U].push_back(
			observation(observation_v2_kind::entity, "entity:fixture", "kind", "function", true));
		rows[5U].push_back(
			observation(observation_v2_kind::type, "type:fixture", "canonical", "int", false));
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

	enum class coverage_mode : std::uint8_t
	{
		exact,
		incomplete,
	};

	class fixture_provider final : public sdk::provider::portable_provider
	{
	  public:
		fixture_provider(std::array<std::vector<sdk::detached_row>, 6U> rows,
						 const coverage_mode coverage)
			: rows_{std::move(rows)}, coverage_{coverage}
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
			return worker_semantics;
		}
		sdk::result<void> run(const sdk::provider::task& task,
							  sdk::provider::context& context) override
		{
			for (std::size_t index{}; index < rows_.size(); ++index)
			{
				const auto& descriptor = task.outputs[index];
				auto output = context.relation(descriptor);
				if (auto begun = output.begin(index < 3U ? "canonical" : "observation",
											  "clang22-atomic",
											  descriptor.id + "-batch");
					!begun)
					return begun;
				for (const auto& row : rows_[index])
					if (auto pushed = output.push(row); !pushed)
						return pushed;
				if (auto ended = output.end(); !ended)
					return ended;
			}
			constexpr std::array<std::string_view, 4U> kinds{
				"cc.call-extraction", "cc.entity", "frontend.clang22.observation", "task"};
			for (const auto kind : kinds)
			{
				context.coverage().request(std::string{kind}, task.task_id);
				const bool incomplete =
					coverage_ == coverage_mode::incomplete && kind == kinds.front();
				if (auto classified =
						context.coverage().classify({std::string{kind},
													 task.task_id,
													 incomplete ? "unresolved" : "covered",
													 incomplete ? "fixture-incomplete" : ""});
					!classified)
					return classified;
			}
			context.evidence().add(
				{"provider.clang22.execution",
				 task.task_id,
				 std::string{id()},
				 coverage_ == coverage_mode::exact ? "exact" : "provider-local"});
			return {};
		}

	  private:
		std::array<std::vector<sdk::detached_row>, 6U> rows_;
		coverage_mode coverage_;
	};

	[[nodiscard]] sdk::result<sealed_materialization_result>
	seal_task(const validated_materialization_request& request,
			  const std::size_t index,
			  const bool empty = false,
			  const coverage_mode coverage = coverage_mode::exact,
			  const bool missing_call = false)
	{
		const auto& task_request = request.tasks[index];
		auto task = reconstruct_provider_task(
			task_request.worker_input, request.output_descriptors, std::string{worker_semantics});
		if (!task)
			return sdk::unexpected(std::move(task.error()));
		fixture_provider provider{fixture_rows(task_request.worker_input, empty, missing_call),
								  coverage};
		transcript_sink sink;
		sdk::provider::protocol_writer writer{sink};
		const sdk::provider::protocol_credit credit{64U * 1024U * 1024U, 65536U};
		writer.grant_credit(credit);
		sdk::provider::execution_context execution;
		execution.budget = task_request.worker_input.budget;
		if (auto run = sdk::provider::run_worker(provider, *task, writer, execution); !run)
			return sdk::unexpected(std::move(run.error()));
		auto frames = sdk::provider::decode_frame_stream(sink.transcript);
		if (!frames)
			return sdk::unexpected(std::move(frames.error()));
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
		if (!validated)
			return sdk::unexpected(std::move(validated.error()));
		if (validated->kind != sdk::provider::detail::transcript_terminal_kind::complete ||
			!validated->sealed() || validated->sealing_error())
			return sdk::unexpected(
				validated->sealing_error().value_or(sdk::error{"fixture.seal-failed", {}, {}}));
		auto seal = std::move(*validated).take_sealed();
		require(seal.has_value(), "generic seal was not movable");
		return validate_and_seal_materialization(task_request, std::move(*seal));
	}

	[[nodiscard]] std::vector<sealed_materialization_result>
	seal_all(const validated_materialization_request& request,
			 const bool empty = false,
			 const coverage_mode coverage = coverage_mode::exact)
	{
		std::vector<sealed_materialization_result> output;
		for (std::size_t index{}; index < request.tasks.size(); ++index)
		{
			auto sealed = seal_task(request, index, empty, coverage);
			require(sealed.has_value(),
					"higher seal failed: " + (sealed ? std::string{} : failure(sealed.error())));
			output.push_back(std::move(*sealed));
		}
		return output;
	}

	[[nodiscard]] std::string incremental_digest(char digit);

	[[nodiscard]] sdk::result<std::vector<std::string>>
	typed_partition_ids(const validated_materialization_request& request,
						const std::size_t task_index,
						const sealed_materialization_result& result,
						const materialization_producer_authority& producer,
						const materialization_guarantee_authority& guarantee)
	{
		auto events = materialization_incremental_result_event_projections(
			request, task_index, result, {}, producer, guarantee);
		if (!events)
			return sdk::unexpected(std::move(events.error()));
		std::vector<std::string> output;
		for (const auto& event : *events)
			if (output.empty() || output.back() != event.partition_id)
				output.push_back(event.partition_id);
		return output;
	}

	[[nodiscard]] sdk::result<materialization_incremental_task_receipt>
	fixture_typed_completeness_receipt(const validated_materialization_request& request,
									   const std::size_t task_index,
									   const sealed_materialization_result& result,
									   const std::span<const std::string> partition_ids,
									   const materialization_producer_authority& producer,
									   const materialization_guarantee_authority& guarantee,
									   std::string provider_sealed_transcript_digest = {})
	{
		if (provider_sealed_transcript_digest.empty())
		{
			auto derived = sdk::provider::detail::provider_sealed_transcript_receipt_digest(
				result.provider_task_id(), "provider.success", result.provider_seal());
			if (!derived)
				return sdk::unexpected(std::move(derived.error()));
			provider_sealed_transcript_digest = std::move(*derived);
		}
		auto events = materialization_incremental_result_event_projections(
			request, task_index, result, partition_ids, producer, guarantee);
		if (!events)
			return sdk::unexpected(std::move(events.error()));
		return make_materialization_incremental_task_receipt(
			request,
			task_index,
			16U,
			incremental_digest('1'),
			static_cast<std::uint64_t>(events->size()),
			"semantic-v2:sha256:" + std::string(64U, '2'),
			std::move(provider_sealed_transcript_digest),
			std::span<const materialization_incremental_event_projection>{*events});
	}

	[[nodiscard]] std::string read_file(const std::filesystem::path& path)
	{
		std::ifstream input{path, std::ios::binary};
		require(input.good(), "failed to open authority file: " + path.string());
		return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
	}

	[[nodiscard]] materialization_producer_authority
	producer_authority(const std::filesystem::path& root)
	{
		materialization_producer_authority output{
			"cxxlens-clang22-materialize",
			"2.0.0",
			"1.0.0",
			"1111111111111111111111111111111111111111",
			"2222222222222222222222222222222222222222",
			{},
		};
		for (const auto path : {
				 "schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml",
				 "schemas/cxxlens_ng_clang22_materialization_contract.yaml",
				 "schemas/cxxlens_ng_clang22_materialization_report.schema.yaml",
				 "schemas/cxxlens_ng_clang22_materialization_request.schema.yaml",
				 "schemas/cxxlens_ng_relation_registry.yaml",
			 })
		{
			const auto bytes = read_file(root / path);
			output.authority_bindings.push_back(
				{path, sdk::content_digest(std::as_bytes(std::span{bytes}))});
		}
		return output;
	}

	[[nodiscard]] std::string request_fixture_json()
	{
		constexpr std::string_view request_json =
			R"cxxlens_json({"engine":{"admitted_descriptors":[{"descriptor_id":"build.compile_unit.v1","runtime_descriptor_digest":"semantic-v2:sha256:1dde734221f3db42a0bdadd531740c35e6f30c15fe196e0b20e1b60c2cf54679"},{"descriptor_id":"build.project.v1","runtime_descriptor_digest":"semantic-v2:sha256:97e5d3d4546803be5de464e5d5de7617b9f4ed29bcb81e503dc6c5a613277cd9"},{"descriptor_id":"build.toolchain_context.v1","runtime_descriptor_digest":"semantic-v2:sha256:3e8895ed57aca936310888a256c4ed31911b46fe5bbac5e045a80f80801cc4e0"},{"descriptor_id":"build.variant.v1","runtime_descriptor_digest":"semantic-v2:sha256:56c59d76bd7921d01c54118470d2643eee5ff8e4ed0ce275f69e9d6ef45500e6"},{"descriptor_id":"cc.call_direct_target.v1","runtime_descriptor_digest":"semantic-v2:sha256:888196009a7344c3cfb198c0c01a359f49e4f042b998d34efc4057c3ba4e56d4"},{"descriptor_id":"cc.call_site.v1","runtime_descriptor_digest":"semantic-v2:sha256:8377b659e3703eef0acb446ab6b07e94aa4655aba33aa5b430e5cf65491163f2"},{"descriptor_id":"cc.entity.v1","runtime_descriptor_digest":"semantic-v2:sha256:4537eb3f074379aa8c2222c9d2ed5dc530340bf1b2b5c862b4cf52b0c37b1b3e"},{"descriptor_id":"frontend.clang22.call_observation.v2","runtime_descriptor_digest":"semantic-v2:sha256:8b79a9fb3d59e750c51310d6f32935701a36c68fd5830228516482b0e7d2cd65"},{"descriptor_id":"frontend.clang22.entity_observation.v2","runtime_descriptor_digest":"semantic-v2:sha256:eb909eec97cec22586f4ac67dc7c56cc29390857df9355186feae5e9ce7700fb"},{"descriptor_id":"frontend.clang22.type_observation.v2","runtime_descriptor_digest":"semantic-v2:sha256:94b6f6efcd46dad74c0cec1c761a2d363c6acdfe135862c37d0b7e28b01b6026"},{"descriptor_id":"source.file.v1","runtime_descriptor_digest":"semantic-v2:sha256:3aebbb05303ba924f1c25547242a656c59d95c265fe99cc3fd77db8633af8609"},{"descriptor_id":"source.span.v1","runtime_descriptor_digest":"semantic-v2:sha256:055e5a6997fef2d1c2dcebfe10baa41813c0ccec091409ad84a1081fd8894a86"}],"engine_generation_id":"engine-generation:sha256:984ec980908d8a3e3d14fb81b06e06009249e909bc7a6d323b447de825da08eb","engine_registry_digest":"semantic-v2:sha256:051823ea2f538bf38656afefb81d22950e5a6ca671fa4d57d89fffd8cfba171a","generation_contract":"cxxlens.clang22-materialization-engine.v2"},"group_topology":{"atomic_output_group":"clang22-atomic","dependency_groups":["canonical","observation"],"partial_policy":"forbid"},"interpretation_policy":{"interpretation_policy_digest":"semantic-v2:sha256:3e97b2cb497e80e0f59953844b4050930e3919f36ac3aab7403d391ab4cc087f","policy_id":"cxxlens.clang22-interpretation-policy.v1","selected_domain":"cc.clang22-canonical-1"},"materialization_request_id":"materialization:semantic-v2:sha256:09a36429bc4dc0f74ef0bf23a6751837d8b0277c06392c9ac5e64c9dab66f95a","project":{"catalog_compile_unit_census_digest":"semantic-v2:sha256:806e8f7964f77dcec9a30078129430a733c89e39488e7fae80b68c7a50d186ba","catalog_compile_units":[{"catalog_compile_unit_id":"catalog-unit:0000","effective_invocation_digest":"semantic-v2:sha256:dd5bdb2f9fd85376546c2f486a1ac3ebeed4bdb922351f3c2f4a7bf89be94acb","environment_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","source_digest":"sha256:deac66ccb79f6d31c0fa7d358de48e083c15c02ff50ec1ebd4b64314b9e6e196"},{"catalog_compile_unit_id":"catalog-unit:0001","effective_invocation_digest":"semantic-v2:sha256:68f779154ad8159b42f2ccc79b7e74999742e0c05a05563421e50d0cae028c09","environment_digest":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","source_digest":"sha256:76af64be58a1f67608cb4c34771305ad773b173cc4cde76261d749928ad4ea49"}],"catalog_digest":"semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","catalog_environment_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","logical_root":"project://fixture","project_id":"project:sha256:9b8cdb2a5afab245af006c61b1bbf0a758687ed969b42d349caf98bcdb6f01c3"},"publication":{"backend":"memory","expected_parent_publication":null,"genesis":true,"partial_policy":"forbid","reopen_before_success":true,"selector":{"catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","channel_id":"channel:clang22-production","condition_universe_id":"condition-universe:one","engine_generation_id":"engine-generation:sha256:984ec980908d8a3e3d14fb81b06e06009249e909bc7a6d323b447de825da08eb","interpretation_policy_digest":"semantic-v2:sha256:3e97b2cb497e80e0f59953844b4050930e3919f36ac3aab7403d391ab4cc087f","relation_registry_digest":"semantic-v2:sha256:051823ea2f538bf38656afefb81d22950e5a6ca671fa4d57d89fffd8cfba171a","trust_policy_digest":"semantic-v2:sha256:a0b190b934d43470d18cbbf326601174fe8a23e52e825c904b8d265dc990d053"},"series_id":"snapshot-series:sha256:c405319c06ab507d9ec7bff97664b5ddac4d211549f4cd72f4fc56621666cdd3","sqlite_path":null,"transaction_count":1},"registry":{"authority_registry_digest":"sha256:4caf626ec6f198118802f22d9cac62b02b2c3bb392fdc8d68b1a58f8101c342e","base_descriptors":[{"contract_digest":"sha256:a0b4b380ab0f5b631fa8ff59c39dcfbd859e26f849d169ae5d6a428e2f9eff5f","descriptor_id":"build.project.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:97e5d3d4546803be5de464e5d5de7617b9f4ed29bcb81e503dc6c5a613277cd9","stage_order":0},{"contract_digest":"sha256:06383e29854c5ce463c996a7a36b6954a4d6388b8384ddc39ad62688bdac0663","descriptor_id":"build.toolchain_context.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:3e8895ed57aca936310888a256c4ed31911b46fe5bbac5e045a80f80801cc4e0","stage_order":1},{"contract_digest":"sha256:1594c6f7ee0f80fdb59f11a9ab45a9521a8aab889052ba3fa40cf1d790aa66a1","descriptor_id":"build.variant.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:56c59d76bd7921d01c54118470d2643eee5ff8e4ed0ce275f69e9d6ef45500e6","stage_order":2},{"contract_digest":"sha256:3c325526160c00ceccd0c43f384689fff95187ef97f926871917ce6b4f7f429a","descriptor_id":"source.file.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:3aebbb05303ba924f1c25547242a656c59d95c265fe99cc3fd77db8633af8609","stage_order":3},{"contract_digest":"sha256:8b019f86c953ce3d08475a726b16dcb355e1474238b6a4300d7dd3dc9fc299b3","descriptor_id":"build.compile_unit.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:1dde734221f3db42a0bdadd531740c35e6f30c15fe196e0b20e1b60c2cf54679","stage_order":4},{"contract_digest":"sha256:645a46ad50ee0c84276ff4e09b2818486bfafe8c631f66368d45aa47cbe659ff","descriptor_id":"source.span.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:055e5a6997fef2d1c2dcebfe10baa41813c0ccec091409ad84a1081fd8894a86","stage_order":5}],"descriptors":[{"atomic_output_group_id":"clang22-atomic","batch_id":"cc.call_direct_target.v1-batch","contract_digest":"sha256:e2960ef9dff7a1190aa6b687281e0b1aeaddfcc684f35a9870323d5716697b2b","dependency_group_id":"canonical","descriptor_id":"cc.call_direct_target.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","runtime_descriptor_digest":"semantic-v2:sha256:888196009a7344c3cfb198c0c01a359f49e4f042b998d34efc4057c3ba4e56d4"},{"atomic_output_group_id":"clang22-atomic","batch_id":"cc.call_site.v1-batch","contract_digest":"sha256:4b8f7b76ef8087485462762bfef006e3fad50354da2738a61402441e9e53510e","dependency_group_id":"canonical","descriptor_id":"cc.call_site.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","runtime_descriptor_digest":"semantic-v2:sha256:8377b659e3703eef0acb446ab6b07e94aa4655aba33aa5b430e5cf65491163f2"},{"atomic_output_group_id":"clang22-atomic","batch_id":"cc.entity.v1-batch","contract_digest":"sha256:89813f031dbe91daed64d5c9d3fa1aef22a1ddcf74cf00a29f292971541f9020","dependency_group_id":"canonical","descriptor_id":"cc.entity.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","runtime_descriptor_digest":"semantic-v2:sha256:4537eb3f074379aa8c2222c9d2ed5dc530340bf1b2b5c862b4cf52b0c37b1b3e"},{"atomic_output_group_id":"clang22-atomic","batch_id":"frontend.clang22.call_observation.v2-batch","contract_digest":"sha256:07ea48a7f00e80972ba59c14ee96f916772ad9ed57fc84e313e3958f08fa548a","dependency_group_id":"observation","descriptor_id":"frontend.clang22.call_observation.v2","descriptor_version":"2.0.0","output_stage":"assertion","runtime_descriptor_digest":"semantic-v2:sha256:8b79a9fb3d59e750c51310d6f32935701a36c68fd5830228516482b0e7d2cd65"},{"atomic_output_group_id":"clang22-atomic","batch_id":"frontend.clang22.entity_observation.v2-batch","contract_digest":"sha256:4a5012801fcde26110a9f6350177d74d7d6975edde96337d4d3918ca7a004d51","dependency_group_id":"observation","descriptor_id":"frontend.clang22.entity_observation.v2","descriptor_version":"2.0.0","output_stage":"assertion","runtime_descriptor_digest":"semantic-v2:sha256:eb909eec97cec22586f4ac67dc7c56cc29390857df9355186feae5e9ce7700fb"},{"atomic_output_group_id":"clang22-atomic","batch_id":"frontend.clang22.type_observation.v2-batch","contract_digest":"sha256:53c54f967eb041e75ea98463c212d259fed0d3a310038ac9c93209749e72387f","dependency_group_id":"observation","descriptor_id":"frontend.clang22.type_observation.v2","descriptor_version":"2.0.0","output_stage":"assertion","runtime_descriptor_digest":"semantic-v2:sha256:94b6f6efcd46dad74c0cec1c761a2d363c6acdfe135862c37d0b7e28b01b6026"}],"path":"schemas/cxxlens_ng_relation_registry.yaml"},"request_digest":"semantic-v2:sha256:09a36429bc4dc0f74ef0bf23a6751837d8b0277c06392c9ac5e64c9dab66f95a","request_version":"2.0.0","schema":"cxxlens.clang22-materialization-request.v2","semantic_request_digest":"semantic-v2:sha256:7d79bd07fade21afe4701e0b55814701792b4b285ab823282e70e229c82e0bdd","tasks":[{"budget":{"address_space_bytes":1073741824,"cpu_ms":10000,"diagnostics":128,"open_files":64,"output_bytes":1048576,"rows":1024,"subprocesses":1,"transport_bytes":2097152,"wall_ms":10000},"build_variant_id":"build-variant:sha256:d0d2c433d8c558923be73e7655f2faa65ea94e330c9aa722d0e7d831d6907e01","catalog_digest":"semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","compile_unit_id":"compile-unit:sha256:be42bfee446b271dd490ce3477e4c2f74e8a6125a6f3ec8a03bbcbe349161e99","condition_id":"condition:all","condition_universe_id":"condition-universe:one","dependency_groups":["canonical","observation"],"effective_argv":["clang++","-std=c++23","project://main.cpp"],"environment_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","interpretation_domain":"cc.clang22-canonical-1","language":"cxx","normalized_invocation_digest":"semantic-v2:sha256:dd5bdb2f9fd85376546c2f486a1ac3ebeed4bdb922351f3c2f4a7bf89be94acb","project_id":"project:sha256:9b8cdb2a5afab245af006c61b1bbf0a758687ed969b42d349caf98bcdb6f01c3","provider_execution_id":"provider-execution:sha256:b7a7f77301033ac30084e3fa657eac3914344e6ab26f3fdc6b52272e10d4c0b3","provider_task_id":"task:semantic-v2:sha256:5fb6b47f3aec5abedd658b2acd86f9a9e3af418712ed9fbf80e30cb3c7306118","requested_descriptor_ids":["cc.call_direct_target.v1","cc.call_site.v1","cc.entity.v1","frontend.clang22.call_observation.v2","frontend.clang22.entity_observation.v2","frontend.clang22.type_observation.v2"],"sandbox":{"minimum":"enforced","policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"selected_catalog_compile_unit_id":"catalog-unit:0000","source":{"content_base64":"aW50IG1haW4oKSB7IHJldHVybiAwOyB9Cg==","content_digest":"sha256:deac66ccb79f6d31c0fa7d358de48e083c15c02ff50ec1ebd4b64314b9e6e196","encoding":"utf8","file_id":"file:sha256:83e065cbf0d8f742fe73a01155b02057c0de0fbe747f88b35ea5e96efe8faf06","line_index_id":"line-index:sha256:99cec457c4ced432a4db1dbb3c30bc291044469abf025737b306ccc7980a3510","logical_path":"project://main.cpp","read_only":false,"size_bytes":25,"source_snapshot_id":"source-snapshot:sha256:cb28d4d99af02e2bf0d1efc7288f211f595ef0a0caeaed889b66cb0fe995086d"},"task_input_digest":"sha256:39bd328764ad9f47c49e0efdfee4d232c410dfa79f096bbed16ea6ca02fd8056","toolchain":{"abi_digest":"sha256:4444444444444444444444444444444444444444444444444444444444444444","builtin_headers_digest":"sha256:3333333333333333333333333333333333333333333333333333333333333333","exact_version":"22.0.0","family":"clang","plugin_spec_digest":"sha256:5555555555555555555555555555555555555555555555555555555555555555","sysroot":null,"target_triple":"x86_64-unknown-linux-gnu"},"toolchain_context_id":"toolchain-context:sha256:78f64803fb0f0f1ab7f10321ebc90aa52aafb99705407b92e005ff7d6ae82b9a","toolchain_digest":"semantic-v2:sha256:d84b82c787577126d2fbbc4e19f1608f77d1725216cf7647c6ace444d1917dbb","variant":{"include_search_digest":"sha256:7777777777777777777777777777777777777777777777777777777777777777","language":"cxx","language_standard":"cxx23","predefined_macros_digest":"sha256:6666666666666666666666666666666666666666666666666666666666666666","semantic_flags_digest":"sha256:8888888888888888888888888888888888888888888888888888888888888888","target_triple":"x86_64-unknown-linux-gnu"},"working_directory":"project://fixture"},{"budget":{"address_space_bytes":1073741824,"cpu_ms":10000,"diagnostics":128,"open_files":64,"output_bytes":1048576,"rows":1024,"subprocesses":1,"transport_bytes":2097152,"wall_ms":10000},"build_variant_id":"build-variant:sha256:d0d2c433d8c558923be73e7655f2faa65ea94e330c9aa722d0e7d831d6907e01","catalog_digest":"semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","compile_unit_id":"compile-unit:sha256:3c5db06dbb85f42d2c2d89246ccf445078097e49250efb894a0270ad2b0cd553","condition_id":"condition:all","condition_universe_id":"condition-universe:one","dependency_groups":["canonical","observation"],"effective_argv":["clang++","-std=c++23","project://unit_1.cpp"],"environment_digest":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","interpretation_domain":"cc.clang22-canonical-1","language":"cxx","normalized_invocation_digest":"semantic-v2:sha256:68f779154ad8159b42f2ccc79b7e74999742e0c05a05563421e50d0cae028c09","project_id":"project:sha256:9b8cdb2a5afab245af006c61b1bbf0a758687ed969b42d349caf98bcdb6f01c3","provider_execution_id":"provider-execution:sha256:8bab585913137bb9106c76c4e46e5934aa07161cffc37784592af415fd7eb784","provider_task_id":"task:semantic-v2:sha256:5fb6b47f3aec5abedd658b2acd86f9a9e3af418712ed9fbf80e30cb3c7306118","requested_descriptor_ids":["cc.call_direct_target.v1","cc.call_site.v1","cc.entity.v1","frontend.clang22.call_observation.v2","frontend.clang22.entity_observation.v2","frontend.clang22.type_observation.v2"],"sandbox":{"minimum":"enforced","policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"selected_catalog_compile_unit_id":"catalog-unit:0001","source":{"content_base64":"aW50IHVuaXRfMSgpIHsgcmV0dXJuIDE7IH0K","content_digest":"sha256:76af64be58a1f67608cb4c34771305ad773b173cc4cde76261d749928ad4ea49","encoding":"utf8","file_id":"file:sha256:c07309e8ee43ccbf4412cd0bbbb99099df12cd1e1a89ac84b7093308ac760b71","line_index_id":"line-index:sha256:5fe3bf322112a1740a8a1e95ed148bc1d8db4ad217b3574d6540c2de296da3a3","logical_path":"project://unit_1.cpp","read_only":false,"size_bytes":27,"source_snapshot_id":"source-snapshot:sha256:3663a8dd373452f9a641715395076673985bd5f2897e7b95983d0888464f9a93"},"task_input_digest":"sha256:6f097c492800c8a785ce8b69ff61ce8c106ab80b296be12c0ae6b92d3017b650","toolchain":{"abi_digest":"sha256:4444444444444444444444444444444444444444444444444444444444444444","builtin_headers_digest":"sha256:3333333333333333333333333333333333333333333333333333333333333333","exact_version":"22.0.0","family":"clang","plugin_spec_digest":"sha256:5555555555555555555555555555555555555555555555555555555555555555","sysroot":null,"target_triple":"x86_64-unknown-linux-gnu"},"toolchain_context_id":"toolchain-context:sha256:78f64803fb0f0f1ab7f10321ebc90aa52aafb99705407b92e005ff7d6ae82b9a","toolchain_digest":"semantic-v2:sha256:d84b82c787577126d2fbbc4e19f1608f77d1725216cf7647c6ace444d1917dbb","variant":{"include_search_digest":"sha256:7777777777777777777777777777777777777777777777777777777777777777","language":"cxx","language_standard":"cxx23","predefined_macros_digest":"sha256:6666666666666666666666666666666666666666666666666666666666666666","semantic_flags_digest":"sha256:8888888888888888888888888888888888888888888888888888888888888888","target_triple":"x86_64-unknown-linux-gnu"},"working_directory":"project://fixture"}],"tool":{"distribution_version":"1.0.0","executable":"cxxlens-clang22-materialize","installed_executable_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","interface_version":"2.0.0","package_configuration":"static","prefix_manifest_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","relocated_prefix_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","source_revision":"1111111111111111111111111111111111111111","source_tree":"2222222222222222222222222222222222222222"},"trust_policy":{"execution_profile":"trust.native-worker","policy_id":"cxxlens.clang22-installed-native-worker-trust.v1","protocol_major":1,"protocol_minor":0,"provider_id":"cxxlens.clang22.reference","provider_version":"1.0.0","required_qualification":"canonical-semantic-qualified","semantic_contract_digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","task_sandbox_requirements":[{"minimum":"enforced","policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}],"trust_policy_digest":"semantic-v2:sha256:a0b190b934d43470d18cbbf326601174fe8a23e52e825c904b8d265dc990d053","worker_sandbox_policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"worker":{"executable":"cxxlens-clang-worker-22","installed_binary_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","protocol_major":1,"protocol_minor":0,"provider_id":"cxxlens.clang22.reference","provider_version":"1.0.0","sandbox_policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","semantic_contract_digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})cxxlens_json";
		return std::string{request_json};
	}

	[[nodiscard]] validated_materialization_request request_fixture()
	{
		auto document = parse_json_object(request_fixture_json());
		require(document.has_value(),
				"request JSON parse failed: " +
					(document ? std::string{} : failure(document.error())));
		auto request = validate_materialization_request(std::move(*document));
		require(request.has_value(),
				"request fixture failed: " + (request ? std::string{} : failure(request.error())));
		return std::move(*request);
	}

	void replace_once(std::string& value, const std::string_view from, const std::string_view to)
	{
		const auto offset = value.find(from);
		require(offset != std::string::npos, "v2.1 fixture replacement target missing");
		value.replace(offset, from.size(), to);
	}

	void replace_all(std::string& value, const std::string_view from, const std::string_view to)
	{
		std::size_t offset{};
		std::size_t count{};
		while ((offset = value.find(from, offset)) != std::string::npos)
		{
			value.replace(offset, from.size(), to);
			offset += to.size();
			++count;
		}
		require(count != 0U, "v2.1 fixture replacement set was empty");
	}

	[[nodiscard]] const std::string& member_string(const json_value& value,
												   const std::string_view name)
	{
		const auto* member = value.member(name);
		require(member != nullptr && member->as_string() != nullptr,
				"v2.1 fixture string member missing");
		return *member->as_string();
	}

	[[nodiscard]] std::string expected_v2_1_trust_digest()
	{
		using sdk::canonical_value;
		auto projection = canonical_value::from_tuple({
			canonical_value::from_string("cxxlens.clang22-installed-native-worker-trust.v1"),
			canonical_value::from_string("trust.native-worker"),
			canonical_value::from_string("cxxlens.clang22.reference"),
			canonical_value::from_string("1.0.0"),
			canonical_value::from_string("sha256:" + std::string(64U, 'a')),
			canonical_value::from_integer(1),
			canonical_value::from_integer(1),
			canonical_value::from_tuple({canonical_value::from_string("task-input-chunks-v1")}),
			canonical_value::from_string("canonical-semantic-qualified"),
			canonical_value::from_string("sha256:" + std::string(64U, 'b')),
			canonical_value::from_tuple({canonical_value::from_tuple({
				canonical_value::from_string("enforced"),
				canonical_value::from_string("sha256:" + std::string(64U, 'b')),
			})}),
		});
		auto encoded = sdk::canonical_binary(projection);
		require(encoded.has_value(), "v2.1 trust projection encoding failed");
		auto digest = sdk::semantic_digest(
			"cxxlens.clang22-installed-native-worker-trust.v1",
			std::string_view{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		require(digest.has_value(), "v2.1 trust projection digest failed");
		return std::move(*digest);
	}

	[[nodiscard]] std::string v2_1_request_fixture_json()
	{
		auto raw = request_fixture_json();
		replace_once(raw, "\"request_version\":\"2.0.0\"", "\"request_version\":\"2.1.0\"");
		replace_once(raw, "\"interface_version\":\"2.0.0\"", "\"interface_version\":\"2.1.0\"");
		replace_once(raw,
					 "\"authority_registry_digest\":\"sha256:"
					 "4caf626ec6f198118802f22d9cac62b02b2c3bb392fdc8d68b1a58f8101c342e\"",
					 "\"authority_registry_digest\":\"sha256:"
					 "e47bbaec6a56bc18f90e314a948003c47bf2c46431cf5f26fd035249a94f35b5\"");
		const auto digest = "sha256:" + std::string(64U, '1');
		replace_once(raw,
					 ",\"prefix_manifest_digest\":\"" + digest +
						 "\",\"relocated_prefix_digest\":\"" + digest + "\"",
					 ",\"occurrence_manifest_digest\":\"" + digest + "\"");
		replace_all(raw,
					"\"protocol_minor\":0",
					"\"protocol_minor\":1,\"required_features\":[\"task-input-chunks-v1\"]");

		auto before = parse_json_object(raw);
		require(before.has_value(), "v2.1 upgraded fixture did not parse");
		const auto* trust = before->root().member("trust_policy");
		const auto* publication = before->root().member("publication");
		require(trust != nullptr && publication != nullptr, "v2.1 fixture globals missing");
		const auto new_trust = expected_v2_1_trust_digest();
		replace_all(raw, member_string(*trust, "trust_policy_digest"), new_trust);

		sdk::snapshot_series_selector selector{
			"catalog:semantic-v2:sha256:"
			"88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464",
			"channel:clang22-production",
			"engine-generation:sha256:"
			"984ec980908d8a3e3d14fb81b06e06009249e909bc7a6d323b447de825da08eb",
			"condition-universe:one",
			"semantic-v2:sha256:051823ea2f538bf38656afefb81d22950e5a6ca671fa4d57d89fffd8cfba171a",
			"semantic-v2:sha256:3e97b2cb497e80e0f59953844b4050930e3919f36ac3aab7403d391ab4cc087f",
			new_trust,
		};
		replace_once(raw, member_string(*publication, "series_id"), selector.id());

		const auto make_identity =
			[](const std::string& value) -> streamed_materialization_request_identity
		{
			auto storage = make_materialization_private_spool();
			require(storage.has_value(), "v2.1 identity spool creation failed");
			require(storage->get()
						->append(std::as_bytes(std::span{value.data(), value.size()}))
						.has_value(),
					"v2.1 identity spool write failed");
			require(storage->get()->seal().has_value(), "v2.1 identity spool seal failed");
			auto index = make_materialization_request_task_index(storage->get()->size_bytes());
			require(index.has_value(), "v2.1 identity task index failed");
			auto envelope = scan_materialization_request_envelope(**storage, {}, index->get());
			require(envelope.has_value(), "v2.1 identity envelope scan failed");
			auto identity =
				derive_streamed_materialization_request_identity(**storage, *envelope, **index);
			require(identity.has_value(), "v2.1 identity derivation failed");
			return std::move(*identity);
		};
		const auto identity = make_identity(raw);
		auto parsed = parse_json_object(raw);
		require(parsed.has_value(), "v2.1 identity fixture parse failed");
		replace_once(raw,
					 member_string(parsed->root(), "materialization_request_id"),
					 identity.materialization_request_id);
		replace_once(raw, member_string(parsed->root(), "request_digest"), identity.request_digest);
		replace_once(raw,
					 member_string(parsed->root(), "semantic_request_digest"),
					 identity.semantic_request_digest);
		return raw;
	}

	[[nodiscard]] sdk::result<validated_materialization_request_v2_1>
	validate_v2_1_request_fixture()
	{
		auto raw = v2_1_request_fixture_json();
		auto storage = make_materialization_private_spool();
		require(storage.has_value(), "v2.1 request spool creation failed");
		require(
			storage->get()->append(std::as_bytes(std::span{raw.data(), raw.size()})).has_value(),
			"v2.1 request spool write failed");
		require(storage->get()->seal().has_value(), "v2.1 request spool seal failed");
		auto index = make_materialization_request_task_index(storage->get()->size_bytes());
		require(index.has_value(), "v2.1 request task index failed");
		auto envelope = scan_materialization_request_envelope(**storage, {}, index->get());
		require(envelope.has_value(), "v2.1 request envelope scan failed");
		return validate_materialization_request_v2_1(
			std::move(*storage), std::move(*envelope), std::move(*index));
	}

	void verify_graph_and_partitions(const validated_materialization_request& request,
									 const sealed_materialization_claims& claims)
	{
		std::set<std::string, std::less<>> hidden_refs;
		std::set<std::string, std::less<>> final_refs;
		std::set<std::string, std::less<>> canonical_final_refs;
		std::map<std::string, std::pair<std::string, std::string>, std::less<>> rows;
		for (const auto& envelope : claims.claim_envelopes())
		{
			const auto row_identity =
				std::pair{envelope.value.descriptor, envelope.value.row.canonical_form()};
			auto [row, inserted] = rows.emplace(envelope.row_ref, row_identity);
			require(inserted || row->second == row_identity,
					"one row reference aliases different descriptor/canonical bytes");
			if (envelope.role == "hidden_precursor")
			{
				require(envelope.value.stage == sdk::claim_stage::assertion,
						"hidden precursor is not an assertion");
				hidden_refs.insert(envelope.claim_ref);
			}
			else
			{
				require(envelope.role == "stored_final", "claim envelope has an unknown role");
				final_refs.insert(envelope.claim_ref);
				if (envelope.value.stage == sdk::claim_stage::canonical_claim)
					canonical_final_refs.insert(envelope.claim_ref);
				else
					require(envelope.value.stage == sdk::claim_stage::assertion,
							"stored final has an unknown SDK claim stage");
			}
		}

		std::set<std::string, std::less<>> edge_precursors;
		std::set<std::string, std::less<>> edge_finals;
		for (const auto& edge : claims.canonicalization_edges())
		{
			require(edge.transform_semantics == claims.canonical_adoption_transform_digest() ||
						edge.transform_semantics == claims.base_ingestion_transform_digest(),
					"canonicalization edge has an unbound transform");
			require(edge_precursors.insert(edge.precursor_claim_ref).second &&
						edge_finals.insert(edge.final_claim_ref).second,
					"canonicalization edges are not one-to-one");
		}
		require(edge_precursors == hidden_refs && edge_finals == canonical_final_refs,
				"canonicalization edge union does not cover exact precursor/final sets");

		std::map<std::string, std::uint64_t, std::less<>> association_counts;
		std::set<std::string, std::less<>> association_ids;
		for (const auto& association : claims.origin_associations())
		{
			require(association_ids.insert(association.association_id).second &&
						final_refs.contains(association.stored_claim_ref),
					"origin association is duplicate or references a non-final claim");
			++association_counts[association.stored_claim_ref];
		}

		std::set<std::string, std::less<>> partition_refs;
		std::uint64_t partition_association_count{};
		std::size_t shared_span_partitions{};
		for (const auto& partition : claims.partitions())
		{
			auto rebuilt = sdk::make_partition_manifest(request.engine, partition.draft);
			require(rebuilt.has_value() && *rebuilt == partition.manifest,
					"partition manifest does not rebind its exact draft");
			const sdk::snapshot_partition_binding expected_binding{
				partition.manifest.partition_id,
				partition.draft.relation_descriptor_id,
				partition.draft.scope,
				partition.draft.condition,
				partition.draft.interpretation,
				partition.draft.producer_semantics,
				partition.draft.producer_input_basis_digest,
				partition.draft.precision_profile,
				partition.draft.assumption_set_id,
			};
			require(partition.binding == expected_binding,
					"partition binding differs from one of its exact eight identity fields");
			auto subject =
				sdk::make_partition_certificate_subject(partition.manifest, partition.binding);
			require(subject.has_value(), "partition identity failed independent SDK rebinding");

			std::set<std::string, std::less<>> draft_refs;
			std::set<std::string, std::less<>> draft_contents;
			for (const auto& claim : partition.draft.claims)
			{
				draft_refs.insert(stored_claim_ref(claim));
				draft_contents.insert(claim.content);
			}
			require(std::vector<std::string>{draft_refs.begin(), draft_refs.end()} ==
							partition.stored_claim_refs &&
						std::vector<std::string>{draft_contents.begin(), draft_contents.end()} ==
							partition.claim_content_ids,
					"partition occurrence/content census differs from its SDK claims");
			for (const auto& claim_ref : partition.stored_claim_refs)
			{
				require(partition_refs.insert(claim_ref).second,
						"one stored final claim occurs in multiple partitions");
				partition_association_count += association_counts.at(claim_ref);
			}
			for (const auto& coverage : partition.draft.coverage)
				require((coverage.domain == "materialization.task" ||
						 coverage.domain == "materialization.dependency-group" ||
						 coverage.domain == "materialization.base-descriptor") &&
							coverage.state == "covered" && coverage.reason.empty(),
						"partition retained noncanonical coverage");
			require(
				partition.empty_partition == partition.draft.claims.empty() &&
					partition.sdk_claim_occurrence_count == draft_refs.size() &&
					partition.origin_association_count ==
						[&]
						{
							std::uint64_t count{};
							for (const auto& claim_ref : partition.stored_claim_refs)
								count += association_counts.at(claim_ref);
							return count;
						}(),
				"partition empty/occurrence/association census differs");
			if (!partition.empty_partition &&
				partition.draft.relation_descriptor_id == "source.span.v1")
			{
				++shared_span_partitions;
				require(
					partition.sdk_claim_occurrence_count == 1U &&
						partition.origin_association_count == 2U,
					"shared call/entity span did not retain one SDK occurrence and two origins");
			}
		}
		require(partition_refs == final_refs &&
					partition_association_count == claims.origin_associations().size() &&
					shared_span_partitions == request.tasks.size(),
				"partition union, association census, or shared-span census differs");

		std::set<std::string, std::less<>> committed_refs;
		for (const auto& claim : claims.final_claim_batch().claims)
			require(committed_refs.insert(stored_claim_ref(claim)).second,
					"committed final claim batch retained duplicate SDK occurrences");
		require(committed_refs == final_refs,
				"single committed claim batch differs from the stored-final envelope union");
	}

	void positive_and_zero_partitions(validated_materialization_request& request,
									  const materialization_producer_authority& producer)
	{
		auto seals = seal_all(request);
		const materialization_guarantee_authority guarantee{
			{}, {"clang22-parse", "query-parity", "store-reopen"}};
		auto claims = construct_materialization_claims(request, seals, producer, guarantee);
		require(claims.has_value(),
				"claim construction failed: " + (claims ? std::string{} : failure(claims.error())));
		require(claims->materializer_semantics_digest() ==
						"semantic-v2:sha256:"
						"31070011864f22e80665a84fe885919a535310fca66d102f2002c2b41313b14f" &&
					claims->direct_basis_digest() ==
						"semantic-v2:sha256:"
						"5b626a30b0742c97b2cdd17cc3b1b25a32cb3f5e38b54908b83b1128548965ee" &&
					claims->canonical_adoption_transform_digest() ==
						"semantic-v2:sha256:"
						"559bacb92b22062b80a3f677d070c4990697f22608171277c10d0402f8fd1e6f" &&
					claims->base_ingestion_transform_digest() ==
						"semantic-v2:sha256:"
						"72602c67a04379f2e20fdc8c5d764aae6c5240c5363b853cf92d11655361985b" &&
					claims->assumption_set_id() ==
						"assumption-set:semantic-v2:sha256:"
						"054f2400cc7d6084286f98ff7c22f4fbcf531178fa605b2211346f528862a098",
				"direct basis, transform, or assumption projection differs from the independent "
				"oracle: " +
					std::string{claims->materializer_semantics_digest()} + "/" +
					std::string{claims->direct_basis_digest()} + "/" +
					std::string{claims->canonical_adoption_transform_digest()} + "/" +
					std::string{claims->base_ingestion_transform_digest()} + "/" +
					std::string{claims->assumption_set_id()});
		require(claims->final_claim_batch().unresolved.empty() &&
					claims->final_claim_batch().conflicts.empty() &&
					claims->final_claim_batch().differential_disagreements.empty(),
				"complete claim batch retained a negative verdict");
		require(!claims->claim_envelopes().empty() && !claims->canonicalization_edges().empty() &&
					!claims->origin_associations().empty() && !claims->partitions().empty(),
				"claim construction omitted a required private projection");
		verify_graph_and_partitions(request, *claims);
		auto store_transaction = make_materialization_store_transaction(request, *claims);
		require(store_transaction.has_value() &&
					store_transaction->draft.series == request.publication.selector &&
					store_transaction->draft.catalog_semantic_digest ==
						request.tasks.front().worker_input.project_catalog.catalog_digest &&
					store_transaction->draft.expected_parent_publication ==
						request.publication.expected_parent_publication &&
					store_transaction->partitions.size() == claims->partitions().size() &&
					store_transaction->closures.empty(),
				"sealed claims did not produce the exact one-transaction Store plan");
		auto streaming_transaction =
			make_materialization_streaming_store_transaction(request, *claims);
		require(streaming_transaction.has_value() &&
					streaming_transaction->draft.series == request.publication.selector &&
					streaming_transaction->draft.catalog_semantic_digest ==
						request.tasks.front().worker_input.project_catalog.catalog_digest,
				"sealed claims did not produce the streaming Store metadata");
		materialization_claim_partition_replay_source partition_source{*claims};
		std::vector<std::string> replayed_partition_ids;
		auto replay = partition_source.replay(
			[&](sdk::partition_draft&& partition) -> sdk::result<void>
			{
				auto manifest = sdk::make_partition_manifest(request.engine, partition);
				if (!manifest)
					return sdk::unexpected(std::move(manifest.error()));
				replayed_partition_ids.push_back(std::move(manifest->partition_id));
				return {};
			});
		require(replay.has_value() &&
					std::ranges::equal(replayed_partition_ids,
									   claims->partitions(),
									   {},
									   std::identity{},
									   [](const auto& partition)
									   {
										   return partition.manifest.partition_id;
									   }),
				"streaming Store source changed the typed partition order or identity");
		const auto first_replay_count = replayed_partition_ids.size();
		replayed_partition_ids.clear();
		replay = partition_source.replay(
			[&](sdk::partition_draft&& partition) -> sdk::result<void>
			{
				auto manifest = sdk::make_partition_manifest(request.engine, partition);
				if (!manifest)
					return sdk::unexpected(std::move(manifest.error()));
				replayed_partition_ids.push_back(std::move(manifest->partition_id));
				return {};
			});
		require(replay.has_value() && replayed_partition_ids.size() == first_replay_count,
				"streaming Store source was not replayable");
		replay = partition_source.replay(
			[](sdk::partition_draft&&) -> sdk::result<void>
			{
				return sdk::unexpected(sdk::error{"test.store-source", "consumer", "rejected"});
			});
		require(!replay && replay.error().code == "test.store-source",
				"streaming Store source swallowed the consumer failure");
		for (std::size_t index = 1U; index < claims->partitions().size(); ++index)
			require(claims->partitions()[index - 1U].manifest.partition_id <
						claims->partitions()[index].manifest.partition_id,
					"partitions are not in deterministic manifest order");
		for (const auto& partition : claims->partitions())
		{
			require(partition.manifest.complete && partition.draft.unresolved.empty() &&
						partition.sdk_claim_occurrence_count ==
							partition.stored_claim_refs.size() &&
						partition.manifest.claim_count == partition.claim_content_ids.size(),
					"partition census or exact coverage differs");
		}

		auto empty_seals = seal_all(request, true);
		auto empty = construct_materialization_claims(request, empty_seals, producer, guarantee);
		require(empty.has_value(),
				"zero-row claim construction failed: " +
					(empty ? std::string{} : failure(empty.error())));
		std::size_t coverage_only{};
		for (const auto& partition : empty->partitions())
			if (partition.empty_partition)
			{
				++coverage_only;
				require(partition.draft.claims.empty() && partition.stored_claim_refs.empty() &&
							partition.claim_content_ids.empty() && partition.manifest.complete,
						"coverage-only zero partition retained a claim or lost coverage");
			}
		require(coverage_only == 7U,
				"zero-row descriptor/task partition census differs: " +
					std::to_string(coverage_only));
	}

	void streaming_source_receipts_replace_resident_payloads(const std::filesystem::path& root)
	{
		auto request = request_fixture();
		auto producer = producer_authority(root);
		auto seals = seal_all(request);
		for (auto& task : request.tasks)
		{
			task.source_receipt = clang22_task_source_receipt{
				task.worker_input.source_size_bytes,
				task.worker_input.source_content_digest,
				task.worker_input.line_index,
			};
			task.worker_input.source.clear();
			task.worker_input.source_content_base64.clear();
			task.worker_payload.clear();
		}
		const materialization_guarantee_authority guarantee{
			{}, {"clang22-parse", "query-parity", "store-reopen"}};
		auto claims = construct_materialization_claims(request, seals, producer, guarantee);
		require(claims.has_value(),
				"sealed source receipt path rejected claims: " +
					(claims ? std::string{} : failure(claims.error())));
	}

	void
	negative_authority_guarantee_order_and_coverage(validated_materialization_request& request,
													materialization_producer_authority producer)
	{
		auto seals = seal_all(request);
		const materialization_guarantee_authority valid_guarantee{
			{}, {"clang22-parse", "query-parity", "store-reopen"}};

		auto reordered_authority = producer;
		std::swap(reordered_authority.authority_bindings[0U],
				  reordered_authority.authority_bindings[1U]);
		auto rejected =
			construct_materialization_claims(request, seals, reordered_authority, valid_guarantee);
		require(!rejected && rejected.error().code == "materialization.identity-mismatch",
				"reordered producer authority was accepted");

		auto duplicate_authority = producer;
		duplicate_authority.authority_bindings[1U] = duplicate_authority.authority_bindings[0U];
		rejected =
			construct_materialization_claims(request, seals, duplicate_authority, valid_guarantee);
		require(!rejected && rejected.error().code == "materialization.identity-mismatch",
				"duplicate/missing producer authority was accepted");

		auto missing_modality = construct_materialization_claims(
			request, seals, producer, materialization_guarantee_authority{{}, {}});
		require(!missing_modality &&
					missing_modality.error().code == "materialization.claim-invalid",
				"missing verification modality was accepted");
		auto duplicate_modality = construct_materialization_claims(
			request,
			seals,
			producer,
			materialization_guarantee_authority{{}, {"clang22-parse", "clang22-parse"}});
		require(!duplicate_modality &&
					duplicate_modality.error().code == "materialization.claim-invalid",
				"duplicate verification modality was accepted");
		auto reordered_modality = construct_materialization_claims(
			request,
			seals,
			producer,
			materialization_guarantee_authority{{}, {"store-reopen", "clang22-parse"}});
		require(!reordered_modality &&
					reordered_modality.error().code == "materialization.claim-invalid",
				"reordered verification modalities were accepted");
		auto invalid_modality = construct_materialization_claims(
			request, seals, producer, materialization_guarantee_authority{{}, {"Unsupported"}});
		require(!invalid_modality &&
					invalid_modality.error().code == "materialization.claim-invalid",
				"non-symbol verification modality was accepted");
		auto extension_modality = construct_materialization_claims(
			request, seals, producer, materialization_guarantee_authority{{}, {"future-modality"}});
		require(extension_modality.has_value(),
				"schema-valid report-owned modality was rejected before authority defines an "
				"allowlist");

		if (seals.size() > 1U)
		{
			std::swap(seals[0U], seals[1U]);
			auto reordered =
				construct_materialization_claims(request, seals, producer, valid_guarantee);
			require(!reordered && reordered.error().code == "materialization.task-binding-mismatch",
					"reordered sealed task results were accepted");
			std::swap(seals[0U], seals[1U]);
		}

		const auto condition = request.tasks.front().worker_input.condition_universe;
		request.tasks.front().worker_input.condition_universe = "condition-universe:drift";
		auto condition_drift =
			construct_materialization_claims(request, seals, producer, valid_guarantee);
		require(!condition_drift &&
					condition_drift.error().code == "materialization.task-binding-mismatch",
				"condition-universe drift was accepted without task rebinding");
		request.tasks.front().worker_input.condition_universe = condition;

		auto incomplete_seals = seal_all(request, false, coverage_mode::incomplete);
		auto incomplete =
			construct_materialization_claims(request, incomplete_seals, producer, valid_guarantee);
		require(!incomplete && incomplete.error().code == "materialization.coverage-incomplete",
				"non-covered provider unit was accepted as exact");

		auto hard_reference = seal_task(request, 0U, false, coverage_mode::exact, true);
		require(!hard_reference && hard_reference.error().code == "materialization.claim-invalid",
				"missing direct-target/call-site hard reference crossed the seal boundary");
	}

	[[nodiscard]] std::vector<std::byte> event_tuple(std::vector<sdk::canonical_value> values)
	{
		auto encoded = sdk::canonical_binary(sdk::canonical_value::from_tuple(std::move(values)));
		require(encoded.has_value(), "event fixture canonical tuple encoding failed");
		return std::move(*encoded);
	}

	void check_partition_event_stream()
	{
		const auto request_id =
			std::string{"materialization:semantic-v2:sha256:"
						"09a36429bc4dc0f74ef0bf23a6751837d8b0277c06392c9ac5e64c9dab66f95a"};
		struct fixture_event
		{
			materialization_partition_event_kind kind;
			std::vector<std::byte> key;
			std::vector<std::byte> payload;
		};
		const auto make_texts = [](const std::initializer_list<std::string_view> values)
		{
			std::vector<sdk::canonical_value> output;
			output.reserve(values.size());
			for (const auto value : values)
				output.push_back(sdk::canonical_value::from_string(std::string{value}));
			return output;
		};
		const auto canonical_bytes = [](sdk::canonical_value value)
		{
			auto encoded = sdk::canonical_binary(value);
			require(encoded.has_value(), "event fixture canonical bytes encoding failed");
			return sdk::canonical_value::from_bytes(std::move(*encoded));
		};
		const auto u64_canonical_bytes = [&](const std::uint64_t value)
		{
			std::vector<std::byte> raw(sizeof(value));
			for (std::size_t index{}; index < raw.size(); ++index)
				raw[index] = static_cast<std::byte>(
					(value >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
			return sdk::canonical_value::from_bytes(std::move(raw));
		};
		const std::vector<fixture_event> events{
			{materialization_partition_event_kind::partition_begin,
			 event_tuple(make_texts({"task", "partition"})),
			 event_tuple(make_texts({"source.span.v1",
									 "project",
									 "condition",
									 "cc",
									 "producer",
									 "basis",
									 "exact",
									 "assumptions"}))},
			{materialization_partition_event_kind::claim_occurrence,
			 event_tuple({sdk::canonical_value::from_string("task"),
						  sdk::canonical_value::from_string("partition"),
						  canonical_bytes(sdk::canonical_value::from_string("claim")),
						  canonical_bytes(sdk::canonical_value::from_string("occurrence"))}),
			 event_tuple({canonical_bytes(sdk::canonical_value::from_string("claim-content")),
						  canonical_bytes(sdk::canonical_value::from_string("occurrence-metadata")),
						  sdk::canonical_value::from_tuple(
							  {canonical_bytes(sdk::canonical_value::from_string("hard"))}),
						  sdk::canonical_value::from_tuple(
							  {canonical_bytes(sdk::canonical_value::from_string("soft"))}),
						  sdk::canonical_value::from_tuple(
							  {canonical_bytes(sdk::canonical_value::from_string("functional"))}),
						  sdk::canonical_value::from_tuple({canonical_bytes(
							  sdk::canonical_value::from_string("differential"))})})},
			{materialization_partition_event_kind::detached_row,
			 event_tuple({sdk::canonical_value::from_string("task"),
						  sdk::canonical_value::from_string("partition"),
						  sdk::canonical_value::from_string("source.span.v1"),
						  canonical_bytes(sdk::canonical_value::from_string("row"))}),
			 event_tuple({canonical_bytes(sdk::canonical_value::from_string("row-bytes"))})},
			{materialization_partition_event_kind::claim_annotation,
			 event_tuple({sdk::canonical_value::from_string("task"),
						  sdk::canonical_value::from_string("partition"),
						  sdk::canonical_value::from_string("claim-content"),
						  canonical_bytes(sdk::canonical_value::from_string("annotation"))}),
			 event_tuple({canonical_bytes(sdk::canonical_value::from_string("annotation-bytes"))})},
			{materialization_partition_event_kind::coverage,
			 event_tuple({sdk::canonical_value::from_string("task"),
						  sdk::canonical_value::from_string("partition"),
						  canonical_bytes(sdk::canonical_value::from_string("coverage"))}),
			 event_tuple({canonical_bytes(sdk::canonical_value::from_string("coverage-bytes"))})},
			{materialization_partition_event_kind::unresolved,
			 event_tuple({sdk::canonical_value::from_string("task"),
						  sdk::canonical_value::from_string("partition"),
						  canonical_bytes(sdk::canonical_value::from_string("unresolved"))}),
			 event_tuple({canonical_bytes(sdk::canonical_value::from_string("unresolved-bytes"))})},
			{materialization_partition_event_kind::partition_end,
			 event_tuple(make_texts({"task", "partition"})),
			 event_tuple({u64_canonical_bytes(7U),
						  u64_canonical_bytes(1U),
						  u64_canonical_bytes(1U),
						  u64_canonical_bytes(1U),
						  u64_canonical_bytes(0U),
						  sdk::canonical_value::from_string("event"),
						  sdk::canonical_value::from_string("claim"),
						  sdk::canonical_value::from_string("row"),
						  sdk::canonical_value::from_string("coverage"),
						  sdk::canonical_value::from_string("unresolved"),
						  sdk::canonical_value::from_string("partition")})},
		};
		std::uint64_t body_bytes{};
		for (const auto& event : events)
		{
			auto size = materialization_partition_event_frame_size(event.key, event.payload);
			require(size.has_value(), "event fixture frame size failed");
			body_bytes += *size;
		}
		auto stream = materialization_partition_event_stream::begin(
			request_id, 7U, {0U, 41U}, events.size(), body_bytes);
		require(stream.has_value(), "event stream begin failed");
		for (const auto& event : events)
			require(stream->append(event.kind, event.key, event.payload).has_value(),
					"event stream append failed");
		auto receipt = std::move(*stream).finalize();
		require(receipt.has_value(), "event stream finalize failed");
		auto spool = std::move(*stream).release_spool();
		require(spool != nullptr, "event stream did not release sealed spool");
		auto replayed = validate_materialization_partition_event_stream(*spool, request_id);
		require(replayed && *replayed == *receipt,
				"event stream replay receipt differs from encoder receipt");
		std::uint64_t replay_count{};
		auto replay = replay_materialization_partition_event_stream(
			*spool,
			request_id,
			[&](const std::uint64_t ordinal,
				const materialization_partition_event_kind,
				const std::span<const std::byte> key,
				const std::span<const std::byte> payload) -> sdk::result<void>
			{
				require(ordinal == replay_count && !key.empty() && !payload.empty(),
						"event replay callback lost exact bounded frame view");
				++replay_count;
				return {};
			});
		require(replay && replay_count == events.size(),
				"event stream bounded replay did not visit the exact frame census");
		const auto typed_invalid_key =
			event_tuple(make_texts({"task", "partition", "claim", "occurrence"}));
		const auto typed_invalid_frame_size =
			*materialization_partition_event_frame_size(typed_invalid_key, events[1].payload);
		auto typed_invalid = materialization_partition_event_stream::begin(
			request_id, 8U, {0U, 0U}, 1U, typed_invalid_frame_size);
		require(typed_invalid.has_value(), "typed event negative setup failed");
		require(!typed_invalid->append(materialization_partition_event_kind::claim_occurrence,
									   typed_invalid_key,
									   events[1].payload),
				"event stream accepted a typed field projection mismatch");

		const auto begin_key = events.front().key;
		const auto begin_payload = events.front().payload;
		auto missing_begin = materialization_partition_event_stream::begin(
			request_id,
			8U,
			{0U, 0U},
			1U,
			*materialization_partition_event_frame_size(begin_key, begin_payload));
		require(missing_begin.has_value(), "negative event stream setup failed");
		require(!missing_begin->append(materialization_partition_event_kind::claim_occurrence,
									   begin_key,
									   begin_payload),
				"event stream accepted a missing partition begin");
		const std::vector<std::byte> noncanonical{std::byte{0x04}};
		auto invalid_tuple =
			materialization_partition_event_stream::begin(request_id, 9U, {0U, 0U}, 1U, 0U);
		require(invalid_tuple.has_value(), "negative tuple stream setup failed");
		require(!invalid_tuple->append(materialization_partition_event_kind::partition_begin,
									   noncanonical,
									   begin_payload),
				"event stream accepted a noncanonical key");
		const auto one_frame =
			*materialization_partition_event_frame_size(begin_key, begin_payload);
		auto census_mismatch = materialization_partition_event_stream::begin(
			request_id, 10U, {0U, 0U}, 1U, one_frame + 1U);
		require(census_mismatch &&
					census_mismatch->append(materialization_partition_event_kind::partition_begin,
											begin_key,
											begin_payload),
				"event stream census negative setup failed");
		auto rejected_finalize = std::move(*census_mismatch).finalize();
		require(!rejected_finalize, "event stream accepted a declared body mismatch");
	}

	[[nodiscard]] std::string incremental_digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] sdk::canonical_value receipt_canonical_bytes(sdk::canonical_value value)
	{
		auto encoded = sdk::canonical_binary(value);
		require(encoded.has_value(), "receipt fixture canonical bytes encoding failed");
		return sdk::canonical_value::from_bytes(std::move(*encoded));
	}

	[[nodiscard]] sdk::canonical_value receipt_u64_bytes(const std::uint64_t value)
	{
		std::vector<std::byte> raw(sizeof(value));
		for (std::size_t index{}; index < raw.size(); ++index)
			raw[index] = static_cast<std::byte>(
				(value >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
		return sdk::canonical_value::from_bytes(std::move(raw));
	}

	[[nodiscard]] std::string execution_journal_digest_for_test(
		const materialization_incremental_execution_journal_receipt& journal)
	{
		std::vector<sdk::canonical_value> task_ids;
		std::vector<sdk::canonical_value> seals;
		task_ids.reserve(journal.canonical_task_ids.size());
		seals.reserve(journal.ordered_task_receipt_seal_digests.size());
		for (const auto& task_id : journal.canonical_task_ids)
			task_ids.push_back(sdk::canonical_value::from_string(task_id));
		for (const auto& seal : journal.ordered_task_receipt_seal_digests)
			seals.push_back(sdk::canonical_value::from_string(seal));
		auto payload = sdk::canonical_binary(sdk::canonical_value::from_tuple({
			sdk::canonical_value::from_string(journal.materialization_request_id),
			receipt_u64_bytes(journal.exact_task_count),
			sdk::canonical_value::from_tuple(std::move(task_ids)),
			sdk::canonical_value::from_tuple(std::move(seals)),
		}));
		require(payload.has_value(), "journal negative fixture encoding failed");
		auto digest = sdk::semantic_digest(
			"cxxlens.df-0200.execution-journal-receipt-set.v1",
			std::string{reinterpret_cast<const char*>(payload->data()), payload->size()});
		require(digest.has_value(), "journal negative fixture digest failed");
		return std::move(*digest);
	}

	[[nodiscard]] std::vector<sdk::canonical_value>
	receipt_texts(const std::initializer_list<std::string_view> values)
	{
		std::vector<sdk::canonical_value> output;
		output.reserve(values.size());
		for (const auto value : values)
			output.push_back(sdk::canonical_value::from_string(std::string{value}));
		return output;
	}

	[[nodiscard]] std::vector<materialization_incremental_event_projection>
	receipt_events(const std::string_view task_id, const std::string_view partition_id)
	{
		const auto task = std::string{task_id};
		const auto partition = std::string{partition_id};
		return {
			{task,
			 partition,
			 materialization_partition_event_kind::partition_begin,
			 event_tuple(receipt_texts({task_id, partition_id})),
			 event_tuple(receipt_texts({"source.span.v1",
										"project",
										"condition",
										"cc",
										"producer",
										"basis",
										"exact",
										"assumptions"}))},
			{task,
			 partition,
			 materialization_partition_event_kind::claim_occurrence,
			 event_tuple(
				 {sdk::canonical_value::from_string(task),
				  sdk::canonical_value::from_string(partition),
				  receipt_canonical_bytes(sdk::canonical_value::from_string("claim")),
				  receipt_canonical_bytes(sdk::canonical_value::from_string("occurrence"))}),
			 event_tuple(
				 {receipt_canonical_bytes(sdk::canonical_value::from_string("claim-content")),
				  receipt_canonical_bytes(sdk::canonical_value::from_string("occurrence-metadata")),
				  sdk::canonical_value::from_tuple(
					  {receipt_canonical_bytes(sdk::canonical_value::from_string("hard"))}),
				  sdk::canonical_value::from_tuple(
					  {receipt_canonical_bytes(sdk::canonical_value::from_string("soft"))}),
				  sdk::canonical_value::from_tuple(
					  {receipt_canonical_bytes(sdk::canonical_value::from_string("functional"))}),
				  sdk::canonical_value::from_tuple({receipt_canonical_bytes(
					  sdk::canonical_value::from_string("differential"))})})},
			{task,
			 partition,
			 materialization_partition_event_kind::detached_row,
			 event_tuple({sdk::canonical_value::from_string(task),
						  sdk::canonical_value::from_string(partition),
						  sdk::canonical_value::from_string("source.span.v1"),
						  receipt_canonical_bytes(sdk::canonical_value::from_string("row"))}),
			 event_tuple(
				 {receipt_canonical_bytes(sdk::canonical_value::from_string("row-bytes"))})},
			{task,
			 partition,
			 materialization_partition_event_kind::claim_annotation,
			 event_tuple(
				 {sdk::canonical_value::from_string(task),
				  sdk::canonical_value::from_string(partition),
				  sdk::canonical_value::from_string("claim-content"),
				  receipt_canonical_bytes(sdk::canonical_value::from_string("annotation"))}),
			 event_tuple(
				 {receipt_canonical_bytes(sdk::canonical_value::from_string("annotation-bytes"))})},
			{task,
			 partition,
			 materialization_partition_event_kind::coverage,
			 event_tuple({sdk::canonical_value::from_string(task),
						  sdk::canonical_value::from_string(partition),
						  receipt_canonical_bytes(sdk::canonical_value::from_string("coverage"))}),
			 event_tuple(
				 {receipt_canonical_bytes(sdk::canonical_value::from_string("coverage-bytes"))})},
			{task,
			 partition,
			 materialization_partition_event_kind::unresolved,
			 event_tuple(
				 {sdk::canonical_value::from_string(task),
				  sdk::canonical_value::from_string(partition),
				  receipt_canonical_bytes(sdk::canonical_value::from_string("unresolved"))}),
			 event_tuple(
				 {receipt_canonical_bytes(sdk::canonical_value::from_string("unresolved-bytes"))})},
			{task,
			 partition,
			 materialization_partition_event_kind::partition_end,
			 event_tuple(receipt_texts({task_id, partition_id})),
			 event_tuple({receipt_u64_bytes(7U),
						  receipt_u64_bytes(1U),
						  receipt_u64_bytes(1U),
						  receipt_u64_bytes(1U),
						  receipt_u64_bytes(0U),
						  sdk::canonical_value::from_string("event"),
						  sdk::canonical_value::from_string("claim"),
						  sdk::canonical_value::from_string("row"),
						  sdk::canonical_value::from_string("coverage"),
						  sdk::canonical_value::from_string("unresolved"),
						  sdk::canonical_value::from_string("partition")})},
		};
	}

	[[nodiscard]] std::unique_ptr<materialization_replayable_spool> make_fixture_partition_spool(
		const std::string_view request_id,
		const std::uint64_t spool_index,
		const std::vector<materialization_incremental_event_projection>& events)
	{
		std::uint64_t body_bytes{};
		for (const auto& event : events)
		{
			auto frame_size = materialization_partition_event_frame_size(event.key, event.payload);
			require(frame_size.has_value(), "ingress fixture frame size failed");
			body_bytes += *frame_size;
		}
		auto stream = materialization_partition_event_stream::begin(
			std::string{request_id}, spool_index, {0U, spool_index}, events.size(), body_bytes);
		require(stream.has_value(), "ingress fixture stream begin failed");
		for (const auto& event : events)
		{
			auto appended = stream->append(event.kind, event.key, event.payload);
			require(appended.has_value(),
					"ingress fixture stream append failed: " +
						(appended ? std::string{}
								  : std::to_string(static_cast<unsigned>(event.kind)) + "/" +
								 failure(appended.error())));
		}
		auto finalized = std::move(*stream).finalize();
		require(finalized.has_value(), "ingress fixture stream finalize failed");
		auto spool = std::move(*stream).release_spool();
		require(spool != nullptr, "ingress fixture stream release failed");
		return spool;
	}

	[[nodiscard]] sdk::result<materialization_incremental_task_receipt>
	fixture_completeness_receipt(const validated_materialization_request& request,
								 const std::size_t task_index,
								 const materialization_incremental_task_binding& binding,
								 const sealed_materialization_result& result,
								 std::string provider_sealed_transcript_digest = {})
	{
		if (provider_sealed_transcript_digest.empty())
		{
			auto derived = sdk::provider::detail::provider_sealed_transcript_receipt_digest(
				result.provider_task_id(), "provider.success", result.provider_seal());
			require(derived.has_value(), "incremental fixture provider seal failed");
			provider_sealed_transcript_digest = std::move(*derived);
		}
		std::vector<std::string> partition_ids;
		partition_ids.reserve(binding.partitions.size());
		for (const auto& partition : binding.partitions)
			partition_ids.push_back(partition.partition_id);
		auto events = materialization_incremental_result_event_projections(
			result, std::span<const std::string>{partition_ids});
		if (!events)
			return sdk::unexpected(std::move(events.error()));
		return make_materialization_incremental_task_receipt(
			request,
			task_index,
			16U,
			incremental_digest('1'),
			static_cast<std::uint64_t>(events->size()),
			"semantic-v2:sha256:" + std::string(64U, '2'),
			std::move(provider_sealed_transcript_digest),
			std::span<const materialization_incremental_event_projection>{*events});
	}

	[[nodiscard]] std::vector<std::unique_ptr<materialization_replayable_spool>>
	fixture_partition_spools(const validated_materialization_request& request,
							 const std::size_t task_index,
							 const materialization_incremental_task_binding& binding,
							 const sealed_materialization_result& result)
	{
		auto request_id = materialization_incremental_request_id(request);
		require(request_id.has_value(), "incremental fixture request identity failed");
		std::vector<std::unique_ptr<materialization_replayable_spool>> output;
		output.reserve(binding.partitions.size());
		for (std::size_t partition_index{}; partition_index < binding.partitions.size();
			 ++partition_index)
		{
			const auto& partition_id = binding.partitions[partition_index].partition_id;
			auto events = materialization_incremental_result_event_projections(
				result, std::span<const std::string>{&partition_id, 1U});
			require(events.has_value(), "incremental fixture oracle projection failed");
			output.push_back(make_fixture_partition_spool(
				*request_id,
				static_cast<std::uint64_t>(task_index * 10U + partition_index),
				*events));
		}
		return output;
	}

	[[nodiscard]] std::vector<std::unique_ptr<materialization_replayable_spool>>
	fixture_typed_partition_spools(const validated_materialization_request& request,
								   const std::size_t task_index,
								   const sealed_materialization_result& result,
								   const std::span<const std::string> partition_ids,
								   const materialization_producer_authority& producer,
								   const materialization_guarantee_authority& guarantee)
	{
		auto request_id = materialization_incremental_request_id(request);
		require(request_id.has_value(), "incremental typed spool request identity failed");
		auto events = materialization_incremental_result_event_projections(
			request, task_index, result, partition_ids, producer, guarantee);
		require(events.has_value(), "incremental typed spool oracle projection failed");
		std::vector<std::unique_ptr<materialization_replayable_spool>> output;
		output.reserve(partition_ids.size());
		for (std::size_t partition_index{}; partition_index < partition_ids.size();
			 ++partition_index)
		{
			std::vector<materialization_incremental_event_projection> partition_events;
			for (const auto& event : *events)
				if (event.partition_id == partition_ids[partition_index])
					partition_events.push_back(event);
			output.push_back(make_fixture_partition_spool(
				*request_id,
				static_cast<std::uint64_t>(task_index * 10U + partition_index),
				partition_events));
		}
		return output;
	}

	[[nodiscard]] materialization_incremental_task_identity
	incremental_identity(const validated_materialization_request& request, std::size_t index);

	void check_incremental_receipts(const validated_materialization_request& request)
	{
		require(request.tasks.size() >= 2U, "receipt fixture needs two selected tasks");
		std::vector<materialization_incremental_task_receipt> receipts;
		receipts.reserve(request.tasks.size());
		for (std::size_t index{}; index < request.tasks.size(); ++index)
		{
			auto events = receipt_events(request.tasks[index].provider_task_id,
										 "partition:receipt-" + std::to_string(index));
			auto receipt = make_materialization_incremental_task_receipt(
				request,
				index,
				16U,
				incremental_digest('1'),
				7U,
				"semantic-v2:sha256:" + std::string(64U, '2'),
				"semantic-v2:sha256:" + std::string(64U, '3'),
				std::span<const materialization_incremental_event_projection>{events});
			require(receipt.has_value(),
					"incremental receipt construction failed: " +
						(receipt ? std::string{} : failure(receipt.error())));
			auto valid =
				validate_materialization_incremental_task_receipt(request, index, *receipt);
			require(valid.has_value(), "incremental receipt self-validation failed");
			receipts.push_back(std::move(*receipt));
		}

		auto request_id = materialization_incremental_request_id(request);
		require(request_id.has_value(), "incremental receipt request identity failed");
		auto journal = seal_materialization_incremental_execution_journal(
			*request_id, std::span<const materialization_incremental_task_receipt>{receipts});
		require(journal.has_value() && journal->exact_task_count == request.tasks.size() &&
					journal->canonical_task_ids.size() == request.tasks.size() &&
					journal->ordered_task_receipt_seal_digests.size() == request.tasks.size(),
				"incremental execution journal seal failed");

		auto duplicate_events =
			receipt_events(request.tasks.front().provider_task_id, "partition:receipt-duplicate");
		duplicate_events.push_back(duplicate_events.front());
		auto duplicate = make_materialization_incremental_task_receipt(
			request,
			0U,
			16U,
			incremental_digest('1'),
			7U,
			"semantic-v2:sha256:" + std::string(64U, '2'),
			"semantic-v2:sha256:" + std::string(64U, '3'),
			std::span<const materialization_incremental_event_projection>{duplicate_events});
		require(!duplicate, "incremental receipt accepted duplicate final event");

		auto missing_boundary =
			receipt_events(request.tasks.front().provider_task_id, "partition:receipt-missing");
		missing_boundary.pop_back();
		auto missing = make_materialization_incremental_task_receipt(
			request,
			0U,
			16U,
			incremental_digest('1'),
			7U,
			"semantic-v2:sha256:" + std::string(64U, '2'),
			"semantic-v2:sha256:" + std::string(64U, '3'),
			std::span<const materialization_incremental_event_projection>{missing_boundary});
		require(!missing, "incremental receipt accepted a whole-partition drop");

		auto wrong_task =
			receipt_events(request.tasks.front().provider_task_id, "partition:receipt-wrong-task");
		wrong_task.front().task_id = "task:receipt-wrong";
		auto wrong = make_materialization_incremental_task_receipt(
			request,
			0U,
			16U,
			incremental_digest('1'),
			7U,
			"semantic-v2:sha256:" + std::string(64U, '2'),
			"semantic-v2:sha256:" + std::string(64U, '3'),
			std::span<const materialization_incremental_event_projection>{wrong_task});
		require(!wrong, "incremental receipt accepted a task identity mismatch");

		auto tampered = receipts.front();
		++tampered.provider_stdout_byte_count;
		auto tampered_validation =
			validate_materialization_incremental_task_receipt(request, 0U, tampered);
		require(!tampered_validation, "incremental receipt accepted a seal mutation");
		auto ordinal_drift = receipts;
		ordinal_drift[1U].canonical_task_ordinal = ordinal_drift[0U].canonical_task_ordinal;
		auto ordinal_drift_journal = seal_materialization_incremental_execution_journal(
			*request_id, std::span<const materialization_incremental_task_receipt>{ordinal_drift});
		require(!ordinal_drift_journal &&
					failure(ordinal_drift_journal.error()) ==
						"materialization.incremental-receipt-invalid/execution-journal/task-order",
				"incremental journal accepted receipt ordinal drift");
		std::swap(receipts[0U], receipts[1U]);
		auto reordered_journal = seal_materialization_incremental_execution_journal(
			*request_id, std::span<const materialization_incremental_task_receipt>{receipts});
		require(!reordered_journal &&
					failure(reordered_journal.error()) ==
						"materialization.incremental-receipt-invalid/execution-journal/task-order",
				"incremental journal accepted reordered task receipts");
	}

	void check_incremental_ingress(const validated_materialization_request& request)
	{
		auto request_id = materialization_incremental_request_id(request);
		require(request_id.has_value(), "ingress fixture request identity failed");
		std::vector<std::vector<std::string>> expected_partitions{
			{"partition:a", "partition:b"},
			{"partition:c"},
		};
		auto ingress = materialization_incremental_ingress::begin(request, expected_partitions);
		require(ingress.has_value(), "incremental ingress begin failed");
		for (std::size_t index{}; index < expected_partitions.size(); ++index)
		{
			std::vector<materialization_incremental_partition_binding> partition_bindings;
			partition_bindings.reserve(expected_partitions[index].size());
			for (const auto& partition_id : expected_partitions[index])
				partition_bindings.emplace_back(partition_id);
			const auto binding = materialization_incremental_task_binding{
				incremental_identity(request, index), std::move(partition_bindings)};
			auto result = seal_task(request, index);
			require(result.has_value(), "ingress fixture task seal failed");
			auto receipt = fixture_completeness_receipt(request, index, binding, *result);
			require(receipt.has_value(), "ingress fixture receipt failed");
			auto spools = fixture_partition_spools(request, index, binding, *result);
			materialization_incremental_task_ingress input{
				std::move(*result), std::move(*receipt), std::move(spools)};
			auto consumed = std::move(*ingress).consume_task(std::move(input));
			require(consumed.has_value(),
					"incremental ingress task consumption failed: " +
						(consumed ? std::string{} : failure(consumed.error())));
		}
		auto journal = std::move(*ingress).finalize();
		require(journal && journal->exact_task_count == request.tasks.size() &&
					journal->ordered_task_receipt_seal_digests.size() == request.tasks.size(),
				"incremental ingress journal finalization failed");

		auto tampered = materialization_incremental_ingress::begin(
			request, std::vector<std::vector<std::string>>{{"partition:a"}, {"partition:b"}});
		require(tampered.has_value(), "ingress stream-tamper setup failed");
		const auto tampered_binding = materialization_incremental_task_binding{
			incremental_identity(request, 0U),
			std::vector<materialization_incremental_partition_binding>{
				materialization_incremental_partition_binding{"partition:a"},
			}};
		auto tampered_result = seal_task(request, 0U);
		require(tampered_result.has_value(), "ingress stream-tamper result failed");
		auto tampered_receipt =
			fixture_completeness_receipt(request, 0U, tampered_binding, *tampered_result);
		const auto& tampered_partition_id = tampered_binding.partitions.front().partition_id;
		auto tampered_events_result = materialization_incremental_result_event_projections(
			*tampered_result, std::span<const std::string>{&tampered_partition_id, 1U});
		require(tampered_events_result.has_value(), "ingress stream-tamper oracle failed");
		auto tampered_events = std::move(*tampered_events_result);
		auto tampered_payload = sdk::canonical_binary_decode(tampered_events[2U].payload);
		require(tampered_payload.has_value() &&
					tampered_payload->type == sdk::canonical_value::kind::ordered_tuple &&
					tampered_payload->tuple.size() == 6U,
				"ingress stream-tamper claim payload decode failed");
		tampered_payload->tuple.front() =
			receipt_canonical_bytes(sdk::canonical_value::from_string("drift"));
		tampered_events[2U].payload = event_tuple(std::move(tampered_payload->tuple));
		std::vector<std::unique_ptr<materialization_replayable_spool>> tampered_spools;
		tampered_spools.push_back(make_fixture_partition_spool(*request_id, 0U, tampered_events));
		require(tampered_receipt.has_value(), "ingress stream-tamper fixture failed");
		materialization_incremental_task_ingress tampered_input{
			std::move(*tampered_result), std::move(*tampered_receipt), std::move(tampered_spools)};
		require(!std::move(*tampered).consume_task(std::move(tampered_input)),
				"incremental ingress accepted an edited event stream");

		auto out_of_order = materialization_incremental_ingress::begin(
			request, std::vector<std::vector<std::string>>{{"partition:a"}, {"partition:b"}});
		require(out_of_order.has_value(), "ingress negative setup failed");
		const auto binding = materialization_incremental_task_binding{
			incremental_identity(request, 1U),
			std::vector<materialization_incremental_partition_binding>{
				materialization_incremental_partition_binding{"partition:b"},
			}};
		auto wrong_result = seal_task(request, 1U);
		require(wrong_result.has_value(), "ingress out-of-order result failed");
		auto wrong_receipt = fixture_completeness_receipt(request, 1U, binding, *wrong_result);
		const auto& wrong_partition_id = binding.partitions.front().partition_id;
		auto wrong_events_result = materialization_incremental_result_event_projections(
			*wrong_result, std::span<const std::string>{&wrong_partition_id, 1U});
		require(wrong_receipt && wrong_events_result, "ingress out-of-order fixture failed");
		std::vector<std::unique_ptr<materialization_replayable_spool>> wrong_spools;
		wrong_spools.push_back(make_fixture_partition_spool(*request_id, 1U, *wrong_events_result));
		materialization_incremental_task_ingress wrong_input{
			std::move(*wrong_result), std::move(*wrong_receipt), std::move(wrong_spools)};
		require(!std::move(*out_of_order).consume_task(std::move(wrong_input)),
				"incremental ingress accepted an out-of-order task");

		auto dropped = materialization_incremental_ingress::begin(
			request, std::vector<std::vector<std::string>>{{"partition:a"}, {"partition:b"}});
		require(dropped.has_value(), "ingress drop setup failed");
		const auto dropped_binding = materialization_incremental_task_binding{
			incremental_identity(request, 0U),
			std::vector<materialization_incremental_partition_binding>{
				materialization_incremental_partition_binding{"partition:a"},
			}};
		auto dropped_result = seal_task(request, 0U);
		auto dropped_receipt =
			fixture_completeness_receipt(request, 0U, dropped_binding, *dropped_result);
		require(dropped_receipt && dropped_result, "ingress drop receipt setup failed");
		materialization_incremental_task_ingress dropped_input{
			std::move(*dropped_result),
			std::move(*dropped_receipt),
			{},
		};
		require(!std::move(*dropped).consume_task(std::move(dropped_input)),
				"incremental ingress accepted a whole-partition drop");
	}

	void check_claim_stream_source(const validated_materialization_request& request)
	{
		const auto maximum = std::numeric_limits<std::uint64_t>::max();
		auto exact_limit = materialization_claim_stream_framed_length(maximum - 9U);
		require(exact_limit && *exact_limit == maximum,
				"claim stream framed length rejected the exact uint64 limit");
		const auto overflow = materialization_claim_stream_framed_length(maximum - 8U);
		require(!overflow &&
					failure(overflow.error()) ==
						"materialization.claim-stream-invalid/digest/length-overflow",
				"claim stream framed length accepted a uint64 overflow");

		std::vector<materialization_claim_stream_task> tasks;
		std::vector<materialization_incremental_task_receipt> receipts;
		tasks.reserve(request.tasks.size());
		receipts.reserve(request.tasks.size());
		for (std::size_t index{}; index < request.tasks.size(); ++index)
		{
			auto result = seal_task(request, index);
			require(result.has_value(), "claim stream task seal failed");
			const auto binding = materialization_incremental_task_binding{
				incremental_identity(request, index),
				std::vector<materialization_incremental_partition_binding>{
					materialization_incremental_partition_binding{"partition:claim-stream-" +
																  std::to_string(index)}}};
			auto receipt = fixture_completeness_receipt(request, index, binding, *result);
			require(receipt.has_value(), "claim stream task receipt construction failed");
			std::vector<std::unique_ptr<materialization_replayable_spool>> spools =
				fixture_partition_spools(request, index, binding, *result);
			receipts.push_back(*receipt);
			tasks.emplace_back(std::move(*receipt), std::move(spools));
		}
		auto request_id = materialization_incremental_request_id(request);
		require(request_id.has_value(), "claim stream request identity failed");
		auto journal = seal_materialization_incremental_execution_journal(
			*request_id, std::span<const materialization_incremental_task_receipt>{receipts});
		require(journal.has_value(), "claim stream journal construction failed");
		std::vector<sdk::canonical_value> expected_task_ids;
		std::vector<sdk::canonical_value> expected_seals;
		expected_task_ids.reserve(receipts.size());
		expected_seals.reserve(receipts.size());
		for (const auto& receipt : receipts)
		{
			expected_task_ids.push_back(sdk::canonical_value::from_string(receipt.task_id));
			expected_seals.push_back(
				sdk::canonical_value::from_string(receipt.pre_encoder_task_receipt_seal_digest));
		}
		auto expected_payload = sdk::canonical_binary(sdk::canonical_value::from_tuple({
			sdk::canonical_value::from_string(*request_id),
			receipt_u64_bytes(static_cast<std::uint64_t>(receipts.size())),
			sdk::canonical_value::from_tuple(std::move(expected_task_ids)),
			sdk::canonical_value::from_tuple(std::move(expected_seals)),
		}));
		require(expected_payload.has_value(), "claim stream journal reference encoding failed");
		auto expected_digest = sdk::semantic_digest(
			"cxxlens.df-0200.execution-journal-receipt-set.v1",
			std::string{reinterpret_cast<const char*>(expected_payload->data()),
						expected_payload->size()});
		require(expected_digest.has_value() &&
					journal->execution_journal_receipt_set_digest == *expected_digest,
				"claim stream journal digest changed its canonical projection");
		require(validate_materialization_incremental_execution_journal(*journal).has_value(),
				"claim stream journal census validator rejected its own journal");
		auto incomplete_journal = *journal;
		incomplete_journal.canonical_task_ids.pop_back();
		auto incomplete_validation =
			validate_materialization_incremental_execution_journal(incomplete_journal);
		require(!incomplete_validation &&
					failure(incomplete_validation.error()) ==
						"materialization.incremental-receipt-invalid/execution-journal/task-census",
				"claim stream journal census validator accepted a missing task identity");
		auto duplicate_seal = *journal;
		duplicate_seal.ordered_task_receipt_seal_digests[1U] =
			duplicate_seal.ordered_task_receipt_seal_digests[0U];
		duplicate_seal.execution_journal_receipt_set_digest =
			execution_journal_digest_for_test(duplicate_seal);
		auto duplicate_seal_validation =
			validate_materialization_incremental_execution_journal(duplicate_seal);
		require(!duplicate_seal_validation &&
					failure(duplicate_seal_validation.error()) ==
						"materialization.incremental-receipt-invalid/execution-journal/task-seal",
				"incremental journal accepted a duplicate receipt seal");
		auto missing_seal = *journal;
		missing_seal.ordered_task_receipt_seal_digests.pop_back();
		auto missing_seal_validation =
			validate_materialization_incremental_execution_journal(missing_seal);
		require(!missing_seal_validation &&
					failure(missing_seal_validation.error()) ==
						"materialization.incremental-receipt-invalid/execution-journal/task-census",
				"claim stream journal census validator accepted a missing receipt seal");
		auto reordered_journal_projection = *journal;
		std::swap(reordered_journal_projection.canonical_task_ids[0U],
				  reordered_journal_projection.canonical_task_ids[1U]);
		std::swap(reordered_journal_projection.ordered_task_receipt_seal_digests[0U],
				  reordered_journal_projection.ordered_task_receipt_seal_digests[1U]);
		reordered_journal_projection.execution_journal_receipt_set_digest =
			execution_journal_digest_for_test(reordered_journal_projection);
		require(validate_materialization_incremental_execution_journal(reordered_journal_projection)
					.has_value(),
				"journal boundary rejected a unique reordered projection before external binding");
		auto external = materialization_claim_stream_source::validate_external_task_receipts(
			request, *journal, std::span<materialization_claim_stream_task>{tasks});
		require(external.has_value(),
				"claim stream external validator rejected unchanged sealed task streams: " +
					(external ? std::string{} : failure(external.error())));
		auto raw_external = materialization_claim_stream_source::validate_external_task_receipts(
			*request_id,
			static_cast<std::uint64_t>(request.tasks.size()),
			*journal,
			std::span<materialization_claim_stream_task>{tasks});
		require(raw_external.has_value(),
				"raw claim stream external validator rejected unchanged sealed task streams: " +
					(raw_external ? std::string{} : failure(raw_external.error())));
		auto reordered_external =
			materialization_claim_stream_source::validate_external_task_receipts(
				request,
				reordered_journal_projection,
				std::span<materialization_claim_stream_task>{tasks});
		require(!reordered_external &&
					failure(reordered_external.error()) ==
						"materialization.claim-stream-invalid/execution-journal/task-binding",
				"external journal binding accepted reordered task receipts");
		std::vector<materialization_claim_stream_task> missing_streams;
		missing_streams.reserve(receipts.size());
		for (const auto& receipt : receipts)
			missing_streams.emplace_back(
				receipt, std::vector<std::unique_ptr<materialization_replayable_spool>>{});
		auto raw_missing_stream =
			materialization_claim_stream_source::validate_external_task_receipts(
				*request_id,
				static_cast<std::uint64_t>(request.tasks.size()),
				*journal,
				std::span<materialization_claim_stream_task>{missing_streams});
		require(
			!raw_missing_stream &&
				failure(raw_missing_stream.error()) ==
					"materialization.claim-stream-invalid/partitions/empty",
			"raw claim stream external validator accepted receipts without sealed event spools");

		auto source =
			materialization_claim_stream_source::begin(request, *journal, std::move(tasks));
		require(source.has_value() && source->task_count() == request.tasks.size() &&
					source->partition_count() == request.tasks.size(),
				"claim stream source did not retain the exact task/partition census");
		const materialization_store_external_authority external_authority{
			&*source,
			&*journal,
		};
		auto external_valid = validate_materialization_store_external_authority(external_authority);
		require(external_valid.has_value(),
				"Store external authority rejected the sealed journal: " +
					(external_valid ? std::string{} : failure(external_valid.error())));
		auto tampered_journal = *journal;
		tampered_journal.execution_journal_receipt_set_digest =
			"semantic-v2:sha256:" + std::string(64U, 'f');
		const materialization_store_external_authority tampered_authority{
			&*source,
			&tampered_journal,
		};
		require(!validate_materialization_store_external_authority(tampered_authority),
				"Store external authority accepted a tampered execution journal");
		std::size_t event_count{};
		auto replayed = source->replay(
			[&](const materialization_claim_stream_event& event) -> sdk::result<void>
			{
				require(!event.task_id.empty() && !event.partition_id.empty() &&
							event.key.size() != 0U,
						"claim stream replay exposed an incomplete event identity");
				++event_count;
				return {};
			});
		require(replayed.has_value() && event_count != 0U,
				"claim stream source did not replay the sealed event boundary");
	}

	[[nodiscard]] incremental::input_fingerprint incremental_fingerprint()
	{
		return {incremental_digest('1'),
				incremental_digest('2'),
				incremental_digest('3'),
				incremental_digest('4'),
				incremental_digest('5'),
				incremental_digest('6'),
				incremental_digest('7'),
				incremental_digest('8'),
				incremental_digest('9'),
				incremental_digest('a'),
				incremental_digest('6'),
				incremental_digest('7'),
				incremental_digest('8'),
				incremental_digest('9'),
				"normalizer-v1",
				incremental_digest('a'),
				incremental_digest('b'),
				"exact"};
	}

	[[nodiscard]] incremental::partition_state incremental_state(std::string id)
	{
		return {std::move(id),
				incremental_fingerprint(),
				incremental_digest('c'),
				incremental_digest('d'),
				false};
	}

	[[nodiscard]] materialization_incremental_task_identity
	incremental_identity(const validated_materialization_request& request, const std::size_t index)
	{
		const auto& task = request.tasks[index];
		return {index,
				task.provider_task_id,
				task.task_input_digest,
				task.worker_input.selected_catalog_compile_unit,
				task.worker_input.compile_unit};
	}

	[[nodiscard]] materialization_incremental_task_binding incremental_binding(
		const validated_materialization_request& request,
		std::string partition_id,
		const std::size_t index,
		incremental::partition_state current,
		std::optional<incremental::partition_state> prior_state,
		std::optional<materialization_incremental_prior_artifact> prior_artifact = std::nullopt)
	{
		if (prior_artifact)
			require(prior_state.has_value(), "incremental fixture prior state missing");
		return {std::move(partition_id),
				incremental_identity(request, index),
				std::optional<incremental::partition_state>{std::move(current)},
				std::move(prior_artifact)};
	}

	[[nodiscard]] materialization_incremental_prior_artifact
	incremental_prior_artifact(incremental::partition_state state,
							   const sealed_materialization_result& result)
	{
		auto digest = seal_materialization_incremental_artifact_digest(result);
		require(digest.has_value(), "incremental fixture prior digest failed");
		return {std::move(state), std::move(*digest)};
	}

	class fixture_incremental_executor final : public materialization_incremental_task_executor
	{
	  public:
		fixture_incremental_executor(const validated_materialization_request& request,
									 const materialization_producer_authority& producer,
									 const materialization_guarantee_authority& guarantee,
									 bool cancelled = false,
									 bool fail = false,
									 bool return_wrong_task = false,
									 std::uint64_t reported_provider_calls = 1U,
									 bool wrong_receipt_digest = false,
									 std::vector<sealed_materialization_result> reusable = {},
									 std::uint64_t reported_reuse_provider_calls = 0U,
									 bool omit_partition_spools = false,
									 coverage_mode coverage = coverage_mode::exact,
									 bool tamper_provider_sealed_transcript_digest = false)
			: request_{request}, producer_{producer}, guarantee_{guarantee}, cancelled_{cancelled},
			  fail_{fail}, return_wrong_task_{return_wrong_task},
			  reported_provider_calls_{reported_provider_calls},
			  wrong_receipt_digest_{wrong_receipt_digest},
			  reported_reuse_provider_calls_{reported_reuse_provider_calls},
			  omit_partition_spools_{omit_partition_spools}, coverage_{coverage},
			  tamper_provider_sealed_transcript_digest_{tamper_provider_sealed_transcript_digest}
		{
			for (auto& result : reusable)
				reusable_.push_back(std::move(result));
		}

		[[nodiscard]] sdk::result<materialization_incremental_task_execution>
		execute(const std::size_t request_task_index,
				const validated_task_request&,
				const materialization_incremental_task_binding&) override
		{
			++calls;
			called_indices.push_back(request_task_index);
			if (fail_)
				return sdk::unexpected(
					sdk::error{"fixture.incremental-provider-failure", "executor", "deliberate"});
			const auto index = return_wrong_task_
				? (request_task_index + 1U) % request_.tasks.size()
				: request_task_index;
			auto result = seal_task(request_, index, false, coverage_);
			if (!result)
				return sdk::unexpected(std::move(result.error()));
			auto digest = seal_materialization_incremental_artifact_digest(*result);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			const auto provider_task_id = std::string{result->provider_task_id()};
			const auto provider_execution_id = std::string{result->provider_execution_id()};
			std::optional<sealed_materialization_result> exact_partition_baseline;
			const sealed_materialization_result* partition_id_source = &*result;
			if (coverage_ == coverage_mode::incomplete)
			{
				// Keep the malformed result under test, but derive its typed partition census from
				// an independent exact result so the fixture reaches the coordinator's oracle.
				auto exact = seal_task(request_, index, false, coverage_mode::exact);
				if (!exact)
					return sdk::unexpected(std::move(exact.error()));
				exact_partition_baseline.emplace(std::move(*exact));
				partition_id_source = &*exact_partition_baseline;
			}
			auto typed_ids =
				typed_partition_ids(request_, index, *partition_id_source, producer_, guarantee_);
			if (!typed_ids)
				return sdk::unexpected(std::move(typed_ids.error()));
			std::vector<std::string> partition_ids = std::move(*typed_ids);
			auto partition_set_digest =
				seal_materialization_incremental_task_partition_set_digest(partition_ids);
			if (!partition_set_digest)
				return sdk::unexpected(std::move(partition_set_digest.error()));
			auto sealed_transcript_digest =
				sdk::provider::detail::provider_sealed_transcript_receipt_digest(
					result->provider_task_id(), "provider.success", result->provider_seal());
			if (!sealed_transcript_digest)
				return sdk::unexpected(std::move(sealed_transcript_digest.error()));
			if (tamper_provider_sealed_transcript_digest_)
				*sealed_transcript_digest = "semantic-v2:sha256:" + std::string(64U, 'f');
			auto completeness = [&]() -> sdk::result<materialization_incremental_task_receipt>
			{
				if (coverage_ == coverage_mode::exact)
					return fixture_typed_completeness_receipt(
						request_,
						index,
						*result,
						std::span<const std::string>{partition_ids},
						producer_,
						guarantee_,
						std::move(*sealed_transcript_digest));
				else
				{
					std::vector<materialization_incremental_partition_binding> partition_bindings;
					partition_bindings.reserve(partition_ids.size());
					for (const auto& partition_id : partition_ids)
						partition_bindings.emplace_back(partition_id);
					const materialization_incremental_task_binding typed_binding{
						incremental_identity(request_, index), std::move(partition_bindings)};
					return fixture_completeness_receipt(request_,
														index,
														typed_binding,
														*result,
														std::move(*sealed_transcript_digest));
				}
			}();
			if (!completeness)
				return sdk::unexpected(std::move(completeness.error()));
			const auto result_artifact_digest = *digest;
			if (wrong_receipt_digest_)
				*digest = incremental_digest('f');
			auto pre_encoder_seal =
				materialization_incremental_pre_encoder_seal{std::move(*completeness),
															 result_artifact_digest,
															 *partition_set_digest,
															 partition_ids};
			auto encode_partition_spools = delayed_encoder(index, partition_ids);
			materialization_incremental_task_execution execution{
				std::move(*result),
				{
					reported_provider_calls_,
					provider_task_id,
					provider_execution_id,
					std::move(*digest),
					std::move(partition_ids),
					std::move(*partition_set_digest),
					std::optional<materialization_incremental_pre_encoder_seal>{
						std::move(pre_encoder_seal)},
				},
				std::move(encode_partition_spools)};
			++returned_executions;
			return execution;
		}

		[[nodiscard]] sdk::result<materialization_incremental_task_reuse>
		load_reusable(const std::size_t request_task_index,
					  const validated_task_request&,
					  const materialization_incremental_task_binding&) override
		{
			++reuse_calls;
			if (request_task_index >= reusable_.size() || !reusable_[request_task_index])
				return sdk::unexpected(
					sdk::error{"fixture.incremental-prior-missing", "cache", "deliberate"});
			auto result = std::move(*reusable_[request_task_index]);
			const auto provider_task_id = std::string{result.provider_task_id()};
			const auto provider_execution_id = std::string{result.provider_execution_id()};
			reusable_[request_task_index].reset();
			auto digest = seal_materialization_incremental_artifact_digest(result);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			auto typed_ids =
				typed_partition_ids(request_, request_task_index, result, producer_, guarantee_);
			if (!typed_ids)
				return sdk::unexpected(std::move(typed_ids.error()));
			std::vector<std::string> partition_ids = std::move(*typed_ids);
			auto partition_set_digest =
				seal_materialization_incremental_task_partition_set_digest(partition_ids);
			if (!partition_set_digest)
				return sdk::unexpected(std::move(partition_set_digest.error()));
			auto sealed_transcript_digest =
				sdk::provider::detail::provider_sealed_transcript_receipt_digest(
					result.provider_task_id(), "provider.success", result.provider_seal());
			if (!sealed_transcript_digest)
				return sdk::unexpected(std::move(sealed_transcript_digest.error()));
			if (tamper_provider_sealed_transcript_digest_)
				*sealed_transcript_digest = "semantic-v2:sha256:" + std::string(64U, 'f');
			auto completeness =
				fixture_typed_completeness_receipt(request_,
												   request_task_index,
												   result,
												   std::span<const std::string>{partition_ids},
												   producer_,
												   guarantee_,
												   std::move(*sealed_transcript_digest));
			if (!completeness)
				return sdk::unexpected(std::move(completeness.error()));
			auto pre_encoder_seal = materialization_incremental_pre_encoder_seal{
				std::move(*completeness), *digest, *partition_set_digest, partition_ids};
			auto encode_partition_spools = delayed_encoder(request_task_index, partition_ids);
			return materialization_incremental_task_reuse{
				std::move(result),
				{
					reported_reuse_provider_calls_,
					std::move(provider_task_id),
					std::move(provider_execution_id),
					std::move(*digest),
					std::move(partition_ids),
					std::move(*partition_set_digest),
					std::optional<materialization_incremental_pre_encoder_seal>{
						std::move(pre_encoder_seal)},
				},
				std::move(encode_partition_spools)};
		}

		[[nodiscard]] bool cancellation_requested() const noexcept override
		{
			return cancelled_;
		}

		[[nodiscard]] bool dynamic_typed_partition_ids() const noexcept override
		{
			return true;
		}

		std::size_t calls{};
		std::size_t reuse_calls{};
		std::size_t returned_executions{};
		std::vector<std::size_t> called_indices;

	  private:
		[[nodiscard]] materialization_incremental_partition_spool_encoder
		delayed_encoder(const std::size_t task_index,
						std::vector<std::string> expected_partition_ids) const
		{
			return
				[this, task_index, expected_partition_ids = std::move(expected_partition_ids)](
					const sealed_materialization_result& result,
					const materialization_incremental_pre_encoder_seal& seal)
					-> sdk::result<std::vector<std::unique_ptr<materialization_replayable_spool>>>
			{
				if (seal.partition_ids != expected_partition_ids)
					return sdk::unexpected(sdk::error{
						"fixture.incremental-encoder", "partition-set", "changed-after-receipt"});
				auto spools = fixture_typed_partition_spools(
					request_,
					task_index,
					result,
					std::span<const std::string>{expected_partition_ids},
					producer_,
					guarantee_);
				if (omit_partition_spools_)
					spools.clear();
				return spools;
			};
		}

		const validated_materialization_request& request_;
		const materialization_producer_authority& producer_;
		const materialization_guarantee_authority& guarantee_;
		bool cancelled_{};
		bool fail_{};
		bool return_wrong_task_{};
		std::uint64_t reported_provider_calls_{1U};
		bool wrong_receipt_digest_{};
		std::uint64_t reported_reuse_provider_calls_{};
		bool omit_partition_spools_{};
		coverage_mode coverage_{coverage_mode::exact};
		bool tamper_provider_sealed_transcript_digest_{};
		std::vector<std::optional<sealed_materialization_result>> reusable_;
	};

	[[nodiscard]] materialization_incremental_task_identity
	v2_1_incremental_identity(validated_materialization_request_v2_1& request,
							  const std::size_t index)
	{
		auto metadata = request.task_metadata(index);
		require(metadata.has_value(), "v2.1 incremental metadata replay failed");
		return {index,
				metadata->provider_task_id,
				metadata->task_input_digest,
				metadata->selected_catalog_compile_unit_id,
				metadata->final_relation_compile_unit_id};
	}

	[[nodiscard]] std::vector<std::string>
	v2_1_partition_ids(const materialization_v2_1_claim_authority& authority,
					   const std::size_t task_index,
					   const materialization_v2_1_task_execution& task,
					   const sealed_materialization_result& result)
	{
		auto events = materialization_incremental_receipt_event_projections(
			authority, task_index, task, result, {});
		require(events.has_value(), "v2.1 receipt partition oracle failed");
		std::set<std::string, std::less<>> seen;
		std::vector<std::string> ids;
		for (const auto& event : *events)
			if (seen.insert(event.partition_id).second)
				ids.push_back(event.partition_id);
		std::ranges::sort(ids);
		require(!ids.empty(), "v2.1 receipt partition census was empty");
		return ids;
	}

	[[nodiscard]] std::vector<std::unique_ptr<materialization_replayable_spool>>
	v2_1_partition_spools(const materialization_v2_1_claim_authority& authority,
						  const std::size_t task_index,
						  const materialization_v2_1_task_execution& task,
						  const sealed_materialization_result& result,
						  const std::span<const std::string> partition_ids)
	{
		auto request_id = materialization_incremental_request_id(authority);
		require(request_id.has_value(), "v2.1 spool request identity failed");
		auto events = materialization_incremental_receipt_event_projections(
			authority, task_index, task, result, {});
		require(events.has_value(), "v2.1 spool event oracle failed");
		std::vector<std::unique_ptr<materialization_replayable_spool>> output;
		output.reserve(partition_ids.size());
		for (std::size_t partition_index{}; partition_index < partition_ids.size();
			 ++partition_index)
		{
			std::vector<materialization_incremental_event_projection> partition_events;
			for (const auto& event : *events)
				if (event.partition_id == partition_ids[partition_index])
					partition_events.push_back(event);
			output.push_back(make_fixture_partition_spool(
				*request_id,
				static_cast<std::uint64_t>(task_index * 10U + partition_index),
				partition_events));
		}
		return output;
	}

	enum class v2_1_receipt_mode
	{
		valid,
		missing_pre_encoder_seal,
		wrong_artifact_digest,
		duplicate_partition_census,
	};

	struct v2_1_output_parts
	{
		sealed_materialization_result result;
		materialization_incremental_provider_execution_receipt receipt;
		materialization_incremental_partition_spool_encoder encode_partition_spools;

		v2_1_output_parts(
			sealed_materialization_result result,
			materialization_incremental_provider_execution_receipt receipt,
			materialization_incremental_partition_spool_encoder encode_partition_spools)
			: result{std::move(result)}, receipt{std::move(receipt)},
			  encode_partition_spools{std::move(encode_partition_spools)}
		{
		}
	};

	class fixture_v2_1_executor final : public materialization_incremental_v2_1_task_executor
	{
	  public:
		fixture_v2_1_executor(
			const materialization_v2_1_claim_authority& authority,
			const materialization_incremental_selected_request_binding_set& binding_set,
			std::vector<sealed_materialization_result> results,
			const v2_1_receipt_mode receipt_mode = v2_1_receipt_mode::valid,
			const bool cancelled = false,
			const bool fail_finalization = false)
			: authority_{authority}, binding_set_{binding_set}, receipt_mode_{receipt_mode},
			  cancelled_{cancelled}, fail_finalization_{fail_finalization}
		{
			results_.reserve(results.size());
			for (auto& result : results)
				results_.emplace_back(std::move(result));
		}

		[[nodiscard]] sdk::result<materialization_incremental_task_execution>
		execute(const std::size_t request_task_index,
				materialization_v2_1_task_execution& task,
				const materialization_incremental_task_binding& binding) override
		{
			++execute_calls;
			called_indices.push_back(request_task_index);
			auto parts = make_output(request_task_index, task, binding, true);
			if (!parts)
				return sdk::unexpected(std::move(parts.error()));
			return materialization_incremental_task_execution{
				std::move(parts->result),
				std::move(parts->receipt),
				std::move(parts->encode_partition_spools)};
		}

		[[nodiscard]] sdk::result<materialization_incremental_task_reuse>
		load_reusable(const std::size_t request_task_index,
					  materialization_v2_1_task_execution& task,
					  const materialization_incremental_task_binding& binding) override
		{
			++reuse_calls;
			auto parts = make_output(request_task_index, task, binding, false);
			if (!parts)
				return sdk::unexpected(std::move(parts.error()));
			return materialization_incremental_task_reuse{
				std::move(parts->result),
				std::move(parts->receipt),
				std::move(parts->encode_partition_spools)};
		}

		[[nodiscard]] bool cancellation_requested() const noexcept override
		{
			return cancelled_;
		}

		[[nodiscard]] bool dynamic_typed_partition_ids() const noexcept override
		{
			return true;
		}

		[[nodiscard]] sdk::result<void> finalize_pending() override
		{
			++finalize_calls;
			finalized = true;
			if (fail_finalization_)
				return sdk::unexpected(
					sdk::error{"fixture.v2-1-finalization-failure", "executor", "deliberate"});
			return {};
		}

		std::size_t execute_calls{};
		std::size_t reuse_calls{};
		std::size_t finalize_calls{};
		std::vector<std::size_t> called_indices;
		bool finalized{};

		[[nodiscard]] sdk::result<v2_1_output_parts>
		make_output(const std::size_t task_index,
					materialization_v2_1_task_execution& task,
					const materialization_incremental_task_binding&,
					const bool recompute)
		{
			if (task_index >= results_.size() || !results_[task_index])
				return sdk::unexpected(
					sdk::error{"fixture.v2-1-result-missing", "executor", "task"});
			auto result = std::move(*results_[task_index]);
			results_[task_index].reset();
			auto artifact_digest = seal_materialization_incremental_artifact_digest(result);
			if (!artifact_digest)
				return sdk::unexpected(std::move(artifact_digest.error()));
			auto events = materialization_incremental_receipt_event_projections(
				authority_, task_index, task, result, {});
			if (!events)
				return sdk::unexpected(std::move(events.error()));
			auto partition_ids = v2_1_partition_ids(authority_, task_index, task, result);
			auto partition_set_digest =
				seal_materialization_incremental_task_partition_set_digest(partition_ids);
			if (!partition_set_digest)
				return sdk::unexpected(std::move(partition_set_digest.error()));
			auto sealed_transcript_digest =
				sdk::provider::detail::provider_sealed_transcript_receipt_digest(
					result.provider_task_id(), "provider.success", result.provider_seal());
			if (!sealed_transcript_digest)
				return sdk::unexpected(std::move(sealed_transcript_digest.error()));
			auto task_receipt = make_materialization_incremental_task_receipt(
				authority_,
				binding_set_,
				task_index,
				task,
				16U,
				incremental_digest('1'),
				static_cast<std::uint64_t>(events->size()),
				"semantic-v2:sha256:" + std::string(64U, '2'),
				std::move(*sealed_transcript_digest),
				std::span<const materialization_incremental_event_projection>{*events});
			if (!task_receipt)
				return sdk::unexpected(std::move(task_receipt.error()));

			std::vector<std::string> covered_partition_ids = partition_ids;
			std::string covered_partition_set_digest = *partition_set_digest;
			if (receipt_mode_ == v2_1_receipt_mode::duplicate_partition_census)
			{
				covered_partition_ids.push_back(covered_partition_ids.back());
				covered_partition_set_digest = incremental_digest('d');
			}
			std::string artifact_receipt_digest = *artifact_digest;
			if (receipt_mode_ == v2_1_receipt_mode::wrong_artifact_digest)
				artifact_receipt_digest = incremental_digest('e');
			std::optional<materialization_incremental_pre_encoder_seal> pre_encoder_seal;
			if (receipt_mode_ != v2_1_receipt_mode::missing_pre_encoder_seal)
				pre_encoder_seal.emplace(std::move(*task_receipt),
										 *artifact_digest,
										 covered_partition_set_digest,
										 covered_partition_ids);
			auto encode_partition_spools =
				[this, task_index, task_ptr = &task](
					const sealed_materialization_result& delayed_result,
					const materialization_incremental_pre_encoder_seal& seal)
				-> sdk::result<std::vector<std::unique_ptr<materialization_replayable_spool>>>
			{
				return v2_1_partition_spools(
					authority_, task_index, *task_ptr, delayed_result, seal.partition_ids);
			};
			materialization_incremental_provider_execution_receipt receipt{
				recompute ? 1U : 0U,
				std::string{result.provider_task_id()},
				std::string{result.provider_execution_id()},
				std::move(artifact_receipt_digest),
				std::move(covered_partition_ids),
				std::move(covered_partition_set_digest),
				std::move(pre_encoder_seal)};
			return v2_1_output_parts{
				std::move(result), std::move(receipt), std::move(encode_partition_spools)};
		}

	  private:
		const materialization_v2_1_claim_authority& authority_;
		const materialization_incremental_selected_request_binding_set& binding_set_;
		v2_1_receipt_mode receipt_mode_{};
		bool cancelled_{};
		bool fail_finalization_{};
		std::vector<std::optional<sealed_materialization_result>> results_;
	};

	struct v2_1_plan_fixture
	{
		incremental::materialization_plan plan;
		std::vector<materialization_incremental_task_binding> bindings;
	};

	[[nodiscard]] v2_1_plan_fixture
	make_v2_1_plan_fixture(validated_materialization_request_v2_1& request,
						   const std::array<std::string, 2U>& artifact_digests,
						   const bool warm_zero)
	{
		auto first = incremental_state("partition:a");
		auto second = incremental_state("partition:b");
		auto current_first = first;
		if (!warm_zero)
			current_first.input.source_digest = incremental_digest('e');
		const std::array candidates{
			incremental::partition_candidate{current_first, first},
			incremental::partition_candidate{second, second},
		};
		auto plan = incremental::make_materialization_plan(candidates);
		require(plan.has_value(), "v2.1 plan fixture construction failed");
		std::vector<materialization_incremental_task_binding> bindings;
		bindings.emplace_back(materialization_incremental_task_binding{
			v2_1_incremental_identity(request, 0U),
			{materialization_incremental_partition_binding{
				"partition:a",
				std::optional<incremental::partition_state>{current_first},
				std::optional<materialization_incremental_prior_artifact>{
					materialization_incremental_prior_artifact{first, artifact_digests[0U]}}}}});
		bindings.emplace_back(materialization_incremental_task_binding{
			v2_1_incremental_identity(request, 1U),
			{materialization_incremental_partition_binding{
				"partition:b",
				std::optional<incremental::partition_state>{second},
				std::optional<materialization_incremental_prior_artifact>{
					materialization_incremental_prior_artifact{second, artifact_digests[1U]}}}}});
		return {std::move(*plan), std::move(bindings)};
	}

	[[nodiscard]] materialization_producer_authority
	v2_1_producer_authority(const std::filesystem::path& root)
	{
		auto output = producer_authority(root);
		output.interface_version = "2.1.0";
		return output;
	}

	[[nodiscard]] std::vector<sealed_materialization_result> v2_1_reference_results()
	{
		auto legacy_request = request_fixture();
		return seal_all(legacy_request);
	}

	void check_incremental_coordinator_v2_1(const std::filesystem::path& root)
	{
		const materialization_guarantee_authority guarantee{{},
															{"clang22.materialization-sealed.v1",
															 "provider.transcript-sealed.v1",
															 "sdk.claim-envelope-validated.v1"}};

		{
			auto accepted = validate_v2_1_request_fixture();
			require(accepted.has_value(), "v2.1 coordinator request admission failed");
			auto authority = make_materialization_v2_1_claim_authority(
				*accepted, v2_1_producer_authority(root), guarantee);
			require(authority.has_value(), "v2.1 coordinator claim authority failed");
			auto binding_set =
				seal_materialization_incremental_selected_request_binding_set(*authority);
			require(binding_set.has_value(), "v2.1 selected binding set seal failed");
			auto results = v2_1_reference_results();
			std::array<std::string, 2U> artifact_digests;
			for (std::size_t index{}; index < results.size(); ++index)
			{
				auto digest = seal_materialization_incremental_artifact_digest(results[index]);
				require(digest.has_value(), "v2.1 reference artifact digest failed");
				artifact_digests[index] = std::move(*digest);
			}
			auto fixture = make_v2_1_plan_fixture(*accepted, artifact_digests, false);
			fixture_v2_1_executor executor{
				*authority, *binding_set, std::move(results), v2_1_receipt_mode::valid};
			auto outcome =
				run_materialization_incremental_coordinator_v2_1(*accepted,
																 fixture.plan,
																 std::move(fixture.bindings),
																 executor,
																 *authority,
																 *binding_set);
			require(outcome.has_value(),
					"v2.1 reuse/recompute coordinator failed: " +
						(outcome ? std::string{} : failure(outcome.error())));
			const auto& census = outcome->execution_census();
			require(census.planned_provider_executions == 1U &&
						census.planned_provider_task_executions == 1U &&
						census.actual_provider_executions == 1U &&
						census.actual_recomputed_partition_count ==
							census.executed_partition_ids.size() &&
						!census.executed_partition_ids.empty() &&
						census.executed_provider_task_ids.size() == 1U &&
						census.executed_provider_execution_ids.size() == 1U &&
						census.executed_artifact_digests.size() == 1U &&
						executor.execute_calls == 1U && executor.reuse_calls == 1U &&
						executor.finalize_calls == 1U && executor.finalized &&
						outcome->bounded_claim_source().sealed() &&
						outcome->claim_stream() != nullptr,
					"v2.1 coordinator lost reuse/recompute census or finalization evidence");
			auto streaming_transaction = make_materialization_streaming_store_transaction(
				*authority, outcome->bounded_claim_source());
			require(streaming_transaction &&
						streaming_transaction->draft.catalog_semantic_digest ==
							authority->catalog()->catalog_digest &&
						outcome->bounded_claim_source().task_count() == authority->task_count(),
					"v2.1 bounded source did not bind the production Store metadata to the exact "
					"request "
					"census");
		}

		{
			auto accepted = validate_v2_1_request_fixture();
			require(accepted.has_value(), "v2.1 plan-state mismatch request admission failed");
			auto authority = make_materialization_v2_1_claim_authority(
				*accepted, v2_1_producer_authority(root), guarantee);
			require(authority.has_value(), "v2.1 plan-state mismatch claim authority failed");
			auto binding_set =
				seal_materialization_incremental_selected_request_binding_set(*authority);
			require(binding_set.has_value(), "v2.1 plan-state mismatch binding set failed");
			auto results = v2_1_reference_results();
			std::array<std::string, 2U> artifact_digests;
			for (std::size_t index{}; index < results.size(); ++index)
			{
				auto digest = seal_materialization_incremental_artifact_digest(results[index]);
				require(digest.has_value(), "v2.1 plan-state mismatch artifact digest failed");
				artifact_digests[index] = std::move(*digest);
			}
			auto fixture = make_v2_1_plan_fixture(*accepted, artifact_digests, false);
			fixture.bindings[0U].partitions.front().current_state =
				incremental_state("partition:a");
			fixture_v2_1_executor executor{
				*authority, *binding_set, std::move(results), v2_1_receipt_mode::valid};
			auto outcome =
				run_materialization_incremental_coordinator_v2_1(*accepted,
																 fixture.plan,
																 std::move(fixture.bindings),
																 executor,
																 *authority,
																 *binding_set);
			require(!outcome && outcome.error().code == "materialization.incremental-invalid" &&
						outcome.error().field == "bindings" &&
						outcome.error().detail == "plan-state-mismatch" &&
						executor.execute_calls == 0U && executor.reuse_calls == 0U &&
						executor.finalize_calls == 0U,
					"v2.1 plan/state mismatch reached the executor");
		}

		{
			auto accepted = validate_v2_1_request_fixture();
			require(accepted.has_value(), "v2.1 cancellation request admission failed");
			auto authority = make_materialization_v2_1_claim_authority(
				*accepted, v2_1_producer_authority(root), guarantee);
			require(authority.has_value(), "v2.1 cancellation claim authority failed");
			auto binding_set =
				seal_materialization_incremental_selected_request_binding_set(*authority);
			require(binding_set.has_value(), "v2.1 cancellation binding set seal failed");
			auto results = v2_1_reference_results();
			std::array<std::string, 2U> artifact_digests;
			for (std::size_t index{}; index < results.size(); ++index)
			{
				auto digest = seal_materialization_incremental_artifact_digest(results[index]);
				require(digest.has_value(), "v2.1 cancellation artifact digest failed");
				artifact_digests[index] = std::move(*digest);
			}
			auto fixture = make_v2_1_plan_fixture(*accepted, artifact_digests, true);
			fixture_v2_1_executor executor{
				*authority, *binding_set, std::move(results), v2_1_receipt_mode::valid, true};
			auto outcome =
				run_materialization_incremental_coordinator_v2_1(*accepted,
																 fixture.plan,
																 std::move(fixture.bindings),
																 executor,
																 *authority,
																 *binding_set);
			require(!outcome && outcome.error().code == "materialization.incremental-invalid" &&
						outcome.error().field == "executor" &&
						outcome.error().detail == "cancelled" && executor.execute_calls == 0U &&
						executor.reuse_calls == 2U && executor.finalize_calls == 1U &&
						executor.finalized,
					"v2.1 cancellation did not preserve finalization lifecycle");
		}

		{
			auto accepted = validate_v2_1_request_fixture();
			require(accepted.has_value(), "v2.1 malformed binding admission failed");
			auto authority = make_materialization_v2_1_claim_authority(
				*accepted, v2_1_producer_authority(root), guarantee);
			require(authority.has_value(), "v2.1 malformed binding authority failed");
			auto binding_set =
				seal_materialization_incremental_selected_request_binding_set(*authority);
			require(binding_set.has_value(), "v2.1 malformed binding set seal failed");
			auto results = v2_1_reference_results();
			std::array<std::string, 2U> artifact_digests;
			for (std::size_t index{}; index < results.size(); ++index)
			{
				auto digest = seal_materialization_incremental_artifact_digest(results[index]);
				require(digest.has_value(), "v2.1 malformed binding artifact digest failed");
				artifact_digests[index] = std::move(*digest);
			}
			auto fixture = make_v2_1_plan_fixture(*accepted, artifact_digests, false);
			fixture.bindings[1U].partitions.front().partition_id = "partition:a";
			fixture.bindings[1U].partitions.front().current_state->partition_id = "partition:a";
			fixture_v2_1_executor executor{
				*authority, *binding_set, std::move(results), v2_1_receipt_mode::valid};
			auto outcome =
				run_materialization_incremental_coordinator_v2_1(*accepted,
																 fixture.plan,
																 std::move(fixture.bindings),
																 executor,
																 *authority,
																 *binding_set);
			require(!outcome && outcome.error().code == "materialization.incremental-invalid" &&
						outcome.error().field == "bindings" && executor.execute_calls == 0U &&
						executor.reuse_calls == 0U && executor.finalize_calls == 0U,
					"v2.1 duplicate/global binding census crossed validation");
		}

		for (const auto mode : {v2_1_receipt_mode::missing_pre_encoder_seal,
								v2_1_receipt_mode::duplicate_partition_census})
		{
			auto accepted = validate_v2_1_request_fixture();
			require(accepted.has_value(), "v2.1 malformed receipt admission failed");
			auto authority = make_materialization_v2_1_claim_authority(
				*accepted, v2_1_producer_authority(root), guarantee);
			require(authority.has_value(), "v2.1 malformed receipt authority failed");
			auto binding_set =
				seal_materialization_incremental_selected_request_binding_set(*authority);
			require(binding_set.has_value(), "v2.1 malformed receipt binding set seal failed");
			auto results = v2_1_reference_results();
			std::array<std::string, 2U> artifact_digests;
			for (std::size_t index{}; index < results.size(); ++index)
			{
				auto digest = seal_materialization_incremental_artifact_digest(results[index]);
				require(digest.has_value(), "v2.1 malformed receipt artifact digest failed");
				artifact_digests[index] = std::move(*digest);
			}
			auto fixture = make_v2_1_plan_fixture(*accepted, artifact_digests, false);
			fixture_v2_1_executor executor{*authority, *binding_set, std::move(results), mode};
			auto outcome =
				run_materialization_incremental_coordinator_v2_1(*accepted,
																 fixture.plan,
																 std::move(fixture.bindings),
																 executor,
																 *authority,
																 *binding_set);
			require(!outcome && outcome.error().code == "materialization.incremental-invalid" &&
						outcome.error().field ==
							(mode == v2_1_receipt_mode::missing_pre_encoder_seal ? "executor"
																				 : "receipt") &&
						executor.execute_calls == 1U && executor.finalize_calls == 0U,
					"v2.1 malformed receipt crossed coordinator validation");
		}
	}

	void check_incremental_coordinator(const validated_materialization_request& request,
									   const materialization_producer_authority& producer)
	{
		require(request.tasks.size() == 2U, "incremental fixture task census changed");
		const materialization_guarantee_authority guarantee{
			{}, {"clang22-parse", "query-parity", "store-reopen"}};
		const auto typed_ids_for_task = [&](const std::size_t task_index)
		{
			auto result = seal_task(request, task_index);
			require(result.has_value(), "typed partition census fixture seal failed");
			auto ids = typed_partition_ids(request, task_index, *result, producer, guarantee);
			require(ids.has_value(), "typed partition census fixture oracle failed");
			return std::move(*ids);
		};
		const auto task_zero_typed_ids = typed_ids_for_task(0U);
		auto first = incremental_state("partition:a");
		auto second = incremental_state("partition:b");
		const auto make_warm_plan = [&]
		{
			const std::array candidates{
				incremental::partition_candidate{second, second},
				incremental::partition_candidate{first, first},
			};
			return incremental::make_materialization_plan(candidates);
		}();
		require(make_warm_plan && make_warm_plan->warm_zero,
				"incremental fixture did not produce warm-zero plan");

		auto prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> warm_bindings;
		warm_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								second,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		warm_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								first,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor warm_executor{
			request, producer, guarantee, false, false, false, 1U, false, std::move(prior)};
		auto warm = run_materialization_incremental_coordinator(
			request, *make_warm_plan, std::move(warm_bindings), warm_executor, producer, guarantee);
		require(warm.has_value(),
				warm ? "warm-zero coordinator failed"
					 : "warm-zero coordinator failed: " + failure(warm.error()));
		require(warm->execution_census().planned_provider_executions == 0U &&
					warm->execution_census().actual_provider_executions == 0U &&
					warm->execution_census().executed_partition_ids.empty() &&
					warm->execution_census().executed_provider_task_ids.empty() &&
					warm->execution_census().executed_provider_execution_ids.empty() &&
					warm->execution_census().executed_artifact_digests.empty() &&
					warm_executor.calls == 0U && warm->bounded_claim_source().sealed() &&
					warm->bounded_claim_source().partition_count() != 0U,
				"warm-zero coordinator executed a provider or lost claims");
		auto full_reference_seals = seal_all(request);
		auto full_reference =
			construct_materialization_claims(request, full_reference_seals, producer, guarantee);
		require(full_reference.has_value(), "full recomputation reference failed");
		auto bounded_status = warm->bounded_claim_source().claim_batch_status();
		require(
			bounded_status &&
				bounded_status->content_digest ==
					full_reference->final_claim_batch().content_digest &&
				bounded_status->claim_count ==
					static_cast<std::uint64_t>(full_reference->final_claim_batch().claims.size()) &&
				bounded_status->unresolved_count ==
					static_cast<std::uint64_t>(
						full_reference->final_claim_batch().unresolved.size()) &&
				bounded_status->conflict_count ==
					static_cast<std::uint64_t>(
						full_reference->final_claim_batch().conflicts.size()) &&
				bounded_status->differential_disagreement_count ==
					static_cast<std::uint64_t>(
						full_reference->final_claim_batch().differential_disagreements.size()) &&
				bounded_status->partition_count == full_reference->partitions().size(),
			"bounded claim status differs from the independent full recomputation");
		auto repeated_bounded_status = warm->bounded_claim_source().claim_batch_status();
		require(repeated_bounded_status &&
					repeated_bounded_status->content_digest == bounded_status->content_digest &&
					repeated_bounded_status->claim_count == bounded_status->claim_count &&
					repeated_bounded_status->unresolved_count == bounded_status->unresolved_count &&
					repeated_bounded_status->partition_count == bounded_status->partition_count,
				"bounded claim status was not replayable");

		std::vector<sdk::partition_draft> warm_partitions;
		std::vector<sdk::unresolved_reference> warm_unresolved;
		auto replay = warm->bounded_claim_source().replay(
			[&](sdk::partition_draft&& partition) -> sdk::result<void>
			{
				warm_unresolved.insert(warm_unresolved.end(),
									   partition.unresolved.begin(),
									   partition.unresolved.end());
				warm_partitions.push_back(std::move(partition));
				return {};
			});
		require(replay && warm_partitions.size() == full_reference->partitions().size(),
				"warm-zero bounded source lost or duplicated partitions");

		std::vector<sdk::claim> warm_claims;
		for (const auto& partition : warm_partitions)
			warm_claims.insert(warm_claims.end(), partition.claims.begin(), partition.claims.end());
		const auto& full_batch = full_reference->final_claim_batch();
		auto warm_digest = sdk::claim_batch_content_digest(
			std::span<const sdk::claim>{warm_claims},
			std::span<const sdk::unresolved_reference>{warm_unresolved},
			std::span<const sdk::claim_conflict>{full_batch.conflicts},
			std::span<const sdk::differential_disagreement>{full_batch.differential_disagreements});
		require(warm_digest && *warm_digest == full_batch.content_digest &&
					warm_claims.size() == full_batch.claims.size(),
				"warm-zero claims differ from the independent full recomputation");
		for (std::size_t index{}; index < warm_partitions.size(); ++index)
		{
			const auto& incremental_partition = warm_partitions[index];
			const auto& full_partition = full_reference->partitions()[index];
			auto incremental_manifest =
				sdk::make_partition_manifest(request.engine, incremental_partition);
			auto bounded_metadata = warm->bounded_claim_source().partition_metadata(
				full_partition.manifest.partition_id);
			require(incremental_manifest && *incremental_manifest == full_partition.manifest &&
						incremental_partition.relation_descriptor_id ==
							full_partition.draft.relation_descriptor_id &&
						incremental_partition.scope == full_partition.draft.scope &&
						incremental_partition.condition == full_partition.draft.condition &&
						incremental_partition.interpretation ==
							full_partition.draft.interpretation &&
						incremental_partition.producer_semantics ==
							full_partition.draft.producer_semantics &&
						incremental_partition.producer_input_basis_digest ==
							full_partition.draft.producer_input_basis_digest &&
						incremental_partition.precision_profile ==
							full_partition.draft.precision_profile &&
						incremental_partition.assumption_set_id ==
							full_partition.draft.assumption_set_id &&
						bounded_metadata &&
						bounded_metadata->stored_claim_refs == full_partition.stored_claim_refs &&
						bounded_metadata->claim_content_ids == full_partition.claim_content_ids &&
						bounded_metadata->sdk_claim_occurrence_count ==
							full_partition.sdk_claim_occurrence_count &&
						bounded_metadata->origin_association_count ==
							full_partition.origin_association_count &&
						bounded_metadata->empty_partition == full_partition.empty_partition,
					"warm-zero partition differs from the independent full recomputation");
		}

		{
			const auto third = incremental_state("partition:c");
			const std::array candidates{
				incremental::partition_candidate{third, third},
				incremental::partition_candidate{second, second},
				incremental::partition_candidate{first, first},
			};
			auto multi_plan = incremental::make_materialization_plan(candidates);
			require(multi_plan && multi_plan->entries.size() == 3U && multi_plan->warm_zero,
					"multi-partition fixture did not produce an exact warm-zero plan");
			auto multi_prior = seal_all(request);
			std::vector<materialization_incremental_task_binding> multi_bindings;
			multi_bindings.emplace_back(materialization_incremental_task_binding{
				incremental_identity(request, 0U),
				{materialization_incremental_partition_binding{
					 "partition:a",
					 std::optional<incremental::partition_state>{first},
					 std::optional<materialization_incremental_prior_artifact>{
						 incremental_prior_artifact(first, multi_prior[0U])}},
				 materialization_incremental_partition_binding{
					 "partition:c",
					 std::optional<incremental::partition_state>{third},
					 std::optional<materialization_incremental_prior_artifact>{
						 incremental_prior_artifact(third, multi_prior[0U])}}}});
			multi_bindings.emplace_back(
				incremental_binding(request,
									"partition:b",
									1U,
									second,
									second,
									std::optional<materialization_incremental_prior_artifact>{
										incremental_prior_artifact(second, multi_prior[1U])}));
			fixture_incremental_executor multi_executor{request,
														producer,
														guarantee,
														false,
														false,
														false,
														1U,
														false,
														std::move(multi_prior)};
			auto multi_warm = run_materialization_incremental_coordinator(request,
																		  *multi_plan,
																		  std::move(multi_bindings),
																		  multi_executor,
																		  producer,
																		  guarantee);
			require(multi_warm &&
						multi_warm->execution_census().planned_provider_executions == 0U &&
						multi_warm->execution_census().actual_provider_executions == 0U &&
						multi_executor.calls == 0U,
					"multi-partition exact reuse crossed the provider boundary");

			auto changed_first = first;
			changed_first.input.source_digest = incremental_digest('e');
			auto changed_third = third;
			changed_third.input.source_digest = incremental_digest('f');
			const std::array affected_candidates{
				incremental::partition_candidate{changed_third, third},
				incremental::partition_candidate{second, second},
				incremental::partition_candidate{changed_first, first},
			};
			auto multi_affected_plan = incremental::make_materialization_plan(affected_candidates);
			require(multi_affected_plan && multi_affected_plan->frontend_provider_executions == 2U,
					"multi-partition fixture did not isolate two affected partitions");
			auto affected_prior = seal_all(request);
			std::vector<materialization_incremental_task_binding> affected_multi_bindings;
			affected_multi_bindings.emplace_back(materialization_incremental_task_binding{
				incremental_identity(request, 0U),
				{materialization_incremental_partition_binding{
					 "partition:a",
					 std::optional<incremental::partition_state>{changed_first},
					 std::optional<materialization_incremental_prior_artifact>{
						 incremental_prior_artifact(first, affected_prior[0U])}},
				 materialization_incremental_partition_binding{
					 "partition:c",
					 std::optional<incremental::partition_state>{changed_third},
					 std::optional<materialization_incremental_prior_artifact>{
						 incremental_prior_artifact(third, affected_prior[0U])}}}});
			affected_multi_bindings.emplace_back(
				incremental_binding(request,
									"partition:b",
									1U,
									second,
									second,
									std::optional<materialization_incremental_prior_artifact>{
										incremental_prior_artifact(second, affected_prior[1U])}));
			fixture_incremental_executor affected_multi_executor{request,
																 producer,
																 guarantee,
																 false,
																 false,
																 false,
																 1U,
																 false,
																 std::move(affected_prior)};
			auto affected_multi =
				run_materialization_incremental_coordinator(request,
															*multi_affected_plan,
															std::move(affected_multi_bindings),
															affected_multi_executor,
															producer,
															guarantee);
			require(affected_multi &&
						affected_multi->execution_census().planned_provider_executions == 2U &&
						affected_multi->execution_census().planned_provider_task_executions == 1U &&
						affected_multi->execution_census().actual_provider_executions == 1U &&
						affected_multi->execution_census().actual_recomputed_partition_count ==
							task_zero_typed_ids.size() &&
						affected_multi->execution_census().executed_partition_ids ==
							task_zero_typed_ids &&
						affected_multi_executor.calls == 1U &&
						affected_multi_executor.called_indices == std::vector<std::size_t>{0U},
					"multi-partition execution receipt did not bind exact affected coverage");

			auto mixed_candidates = std::array{
				incremental::partition_candidate{changed_first, first},
				incremental::partition_candidate{second, second},
				incremental::partition_candidate{third, third},
			};
			auto mixed_plan = incremental::make_materialization_plan(mixed_candidates);
			require(mixed_plan && mixed_plan->frontend_provider_executions == 1U,
					"mixed-task fixture did not isolate one recompute partition");
			auto mixed_prior = seal_all(request);
			std::vector<materialization_incremental_task_binding> mixed_bindings;
			mixed_bindings.emplace_back(materialization_incremental_task_binding{
				incremental_identity(request, 0U),
				{materialization_incremental_partition_binding{
					 "partition:a",
					 std::optional<incremental::partition_state>{changed_first},
					 std::optional<materialization_incremental_prior_artifact>{
						 incremental_prior_artifact(first, mixed_prior[0U])}},
				 materialization_incremental_partition_binding{
					 "partition:c",
					 std::optional<incremental::partition_state>{third},
					 std::optional<materialization_incremental_prior_artifact>{
						 incremental_prior_artifact(third, mixed_prior[0U])}}}});
			mixed_bindings.emplace_back(
				incremental_binding(request,
									"partition:b",
									1U,
									second,
									second,
									std::optional<materialization_incremental_prior_artifact>{
										incremental_prior_artifact(second, mixed_prior[1U])}));
			fixture_incremental_executor mixed_executor{request,
														producer,
														guarantee,
														false,
														false,
														false,
														1U,
														false,
														std::move(mixed_prior)};
			auto mixed = run_materialization_incremental_coordinator(request,
																	 *mixed_plan,
																	 std::move(mixed_bindings),
																	 mixed_executor,
																	 producer,
																	 guarantee);
			require(!mixed && mixed.error().code == "materialization.incremental-invalid" &&
						mixed_executor.calls == 0U,
					"mixed recompute/reuse task crossed the provider boundary");
		}

		{
			auto bad_reuse_prior = seal_all(request);
			std::vector<materialization_incremental_task_binding> bad_reuse_bindings;
			bad_reuse_bindings.emplace_back(
				incremental_binding(request,
									"partition:b",
									1U,
									second,
									second,
									std::optional<materialization_incremental_prior_artifact>{
										incremental_prior_artifact(second, bad_reuse_prior[1U])}));
			bad_reuse_bindings.emplace_back(
				incremental_binding(request,
									"partition:a",
									0U,
									first,
									first,
									std::optional<materialization_incremental_prior_artifact>{
										incremental_prior_artifact(first, bad_reuse_prior[0U])}));
			fixture_incremental_executor bad_reuse_executor{request,
															producer,
															guarantee,
															false,
															false,
															false,
															1U,
															false,
															std::move(bad_reuse_prior),
															1U};
			auto bad_reuse =
				run_materialization_incremental_coordinator(request,
															*make_warm_plan,
															std::move(bad_reuse_bindings),
															bad_reuse_executor,
															producer,
															guarantee);
			require(!bad_reuse && bad_reuse.error().code == "materialization.incremental-invalid" &&
						bad_reuse_executor.calls == 0U,
					"nonzero reuse provider-call receipt was accepted");
		}

		auto changed = first;
		changed.input.source_digest = incremental_digest('e');
		auto unchanged = second;
		const std::array affected_candidates{
			incremental::partition_candidate{changed, incremental_state("partition:a")},
			incremental::partition_candidate{unchanged, unchanged},
		};
		auto affected_plan = incremental::make_materialization_plan(affected_candidates);
		require(affected_plan && affected_plan->frontend_provider_executions == 1U,
				"incremental fixture did not isolate one changed partition");
		require(affected_plan->entries.front().planner_binding.has_value(),
				"incremental planner did not retain the exact candidate binding");
		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> affected_bindings;
		affected_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								unchanged,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		affected_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								changed,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor affected_executor{
			request, producer, guarantee, false, false, false, 1U, false, std::move(prior)};
		auto affected = run_materialization_incremental_coordinator(request,
																	*affected_plan,
																	std::move(affected_bindings),
																	affected_executor,
																	producer,
																	guarantee);
		require(affected && affected->execution_census().actual_provider_executions == 1U &&
					affected->execution_census().executed_partition_ids == task_zero_typed_ids &&
					affected->execution_census().executed_provider_task_ids ==
						std::vector<std::string>{request.tasks[0].provider_task_id} &&
					affected->execution_census().executed_provider_execution_ids ==
						std::vector<std::string>{request.tasks[0].provider_execution_id} &&
					affected->execution_census().executed_artifact_digests.size() == 1U &&
					affected_executor.calls == 1U &&
					affected_executor.called_indices == std::vector<std::size_t>{0U},
				"incremental coordinator executed an unrelated partition or lost order");

		{
			auto invalid_current = first;
			invalid_current.input.source_digest = incremental_digest('e');
			const std::array invalid_candidates{
				incremental::partition_candidate{invalid_current, first},
				incremental::partition_candidate{second, second},
			};
			auto invalid_plan = incremental::make_materialization_plan(invalid_candidates);
			require(invalid_plan && invalid_plan->frontend_provider_executions == 1U,
					"plan/state mismatch fixture plan construction failed");
			auto invalid_prior = seal_all(request);
			std::vector<materialization_incremental_task_binding> invalid_bindings;
			invalid_bindings.emplace_back(
				incremental_binding(request,
									"partition:b",
									1U,
									second,
									second,
									std::optional<materialization_incremental_prior_artifact>{
										incremental_prior_artifact(second, invalid_prior[1U])}));
			invalid_bindings.emplace_back(
				incremental_binding(request,
									"partition:a",
									0U,
									first,
									first,
									std::optional<materialization_incremental_prior_artifact>{
										incremental_prior_artifact(first, invalid_prior[0U])}));
			fixture_incremental_executor invalid_executor{request,
														  producer,
														  guarantee,
														  false,
														  false,
														  false,
														  1U,
														  false,
														  std::move(invalid_prior)};
			auto invalid = run_materialization_incremental_coordinator(request,
																	   *invalid_plan,
																	   std::move(invalid_bindings),
																	   invalid_executor,
																	   producer,
																	   guarantee);
			require(!invalid && invalid.error().code == "materialization.incremental-invalid" &&
						invalid.error().field == "bindings" &&
						invalid.error().detail == "plan-state-mismatch" &&
						invalid_executor.calls == 0U && invalid_executor.reuse_calls == 0U,
					"plan/state mismatch reached the executor");
		}

		{
			auto planner_current = first;
			planner_current.input.source_digest = incremental_digest('e');
			auto binding_current = first;
			binding_current.input.source_digest = incremental_digest('f');
			const std::array lossy_candidates{
				incremental::partition_candidate{planner_current, first},
				incremental::partition_candidate{second, second},
			};
			auto lossy_plan = incremental::make_materialization_plan(lossy_candidates);
			require(lossy_plan &&
						lossy_plan->entries.front().decision == incremental::action::recompute &&
						lossy_plan->entries.front().reason == "sdk.incremental-source-changed",
					"lossy planner binding fixture did not produce the expected decision/reason");
			auto lossy_prior = seal_all(request);
			std::vector<materialization_incremental_task_binding> lossy_bindings;
			lossy_bindings.emplace_back(
				incremental_binding(request,
									"partition:b",
									1U,
									second,
									second,
									std::optional<materialization_incremental_prior_artifact>{
										incremental_prior_artifact(second, lossy_prior[1U])}));
			lossy_bindings.emplace_back(
				incremental_binding(request,
									"partition:a",
									0U,
									binding_current,
									first,
									std::optional<materialization_incremental_prior_artifact>{
										incremental_prior_artifact(first, lossy_prior[0U])}));
			fixture_incremental_executor lossy_executor{request,
														producer,
														guarantee,
														false,
														false,
														false,
														1U,
														false,
														std::move(lossy_prior)};
			auto lossy = run_materialization_incremental_coordinator(request,
																	 *lossy_plan,
																	 std::move(lossy_bindings),
																	 lossy_executor,
																	 producer,
																	 guarantee);
			require(!lossy && lossy.error().code == "materialization.incremental-invalid" &&
						lossy.error().field == "bindings" &&
						lossy.error().detail == "plan-state-mismatch" &&
						lossy_executor.calls == 0U && lossy_executor.reuse_calls == 0U,
					"lossy decision/reason comparison accepted a different candidate state");
		}

		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> missing_prior;
		missing_prior.emplace_back(
			incremental_binding(request, "partition:a", 0U, first, std::nullopt));
		missing_prior.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								second,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		fixture_incremental_executor missing_executor{
			request, producer, guarantee, false, false, false, 1U, false, std::move(prior)};
		auto missing = run_materialization_incremental_coordinator(request,
																   *make_warm_plan,
																   std::move(missing_prior),
																   missing_executor,
																   producer,
																   guarantee);
		require(!missing && missing.error().code == "materialization.incremental-invalid" &&
					missing_executor.calls == 0U,
				"missing sealed prior artifact was not rejected before execution");

		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> cancelled_bindings;
		cancelled_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								unchanged,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		cancelled_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								changed,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor cancelled_executor{
			request, producer, guarantee, true, false, false, 1U, false, std::move(prior)};
		auto cancelled = run_materialization_incremental_coordinator(request,
																	 *affected_plan,
																	 std::move(cancelled_bindings),
																	 cancelled_executor,
																	 producer,
																	 guarantee);
		require(!cancelled && cancelled.error().code == "materialization.incremental-invalid" &&
					cancelled_executor.calls == 0U,
				"cancelled incremental execution crossed the provider boundary");

		std::vector<materialization_incremental_task_binding> duplicate_bindings;
		duplicate_bindings.emplace_back(
			incremental_binding(request, "partition:a", 0U, first, std::nullopt));
		duplicate_bindings.emplace_back(incremental_binding(
			request, "partition:a", 1U, incremental_state("partition:a"), std::nullopt));
		fixture_incremental_executor duplicate_executor{request, producer, guarantee};
		auto duplicate = run_materialization_incremental_coordinator(request,
																	 *make_warm_plan,
																	 std::move(duplicate_bindings),
																	 duplicate_executor,
																	 producer,
																	 guarantee);
		require(!duplicate && duplicate.error().code == "materialization.incremental-invalid" &&
					duplicate_executor.calls == 0U,
				"duplicate/missing incremental binding crossed validation");

		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> failed_bindings;
		failed_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								unchanged,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		failed_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								changed,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor failed_executor{
			request, producer, guarantee, false, true, false, 1U, false, std::move(prior)};
		auto failed = run_materialization_incremental_coordinator(request,
																  *affected_plan,
																  std::move(failed_bindings),
																  failed_executor,
																  producer,
																  guarantee);
		require(!failed && failed.error().code == "fixture.incremental-provider-failure" &&
					failed_executor.calls == 1U,
				"provider failure was converted into a successful incremental result");

		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> wrong_bindings;
		wrong_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								unchanged,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		wrong_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								changed,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor wrong_executor{
			request, producer, guarantee, false, false, true, 1U, false, std::move(prior)};
		auto wrong = run_materialization_incremental_coordinator(request,
																 *affected_plan,
																 std::move(wrong_bindings),
																 wrong_executor,
																 producer,
																 guarantee);
		require(!wrong && wrong.error().code == "materialization.incremental-invalid" &&
					wrong_executor.calls == 1U,
				"wrong-task sealed output crossed the coordinator binding boundary");

		prior = seal_all(request);
		auto swapped_b =
			incremental_binding(request,
								"partition:b",
								1U,
								second,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])});
		swapped_b.task_identity = incremental_identity(request, 0U);
		std::vector<materialization_incremental_task_binding> swapped_bindings;
		swapped_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								first,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		swapped_bindings.emplace_back(std::move(swapped_b));
		fixture_incremental_executor swapped_executor{
			request, producer, guarantee, false, false, false, 1U, false, std::move(prior)};
		auto swapped = run_materialization_incremental_coordinator(request,
																   *make_warm_plan,
																   std::move(swapped_bindings),
																   swapped_executor,
																   producer,
																   guarantee);
		require(!swapped && swapped.error().code == "materialization.incremental-invalid" &&
					swapped_executor.calls == 0U,
				"swapped typed task identity crossed the coordinator boundary");

		prior = seal_all(request);
		auto corrupt_b =
			incremental_binding(request,
								"partition:b",
								1U,
								second,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])});
		corrupt_b.partitions.front().prior_artifact->state.corruption_detected = true;
		std::vector<materialization_incremental_task_binding> corrupt_bindings;
		corrupt_bindings.emplace_back(std::move(corrupt_b));
		corrupt_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								first,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor corrupt_executor{
			request, producer, guarantee, false, false, false, 1U, false, std::move(prior)};
		auto corrupt = run_materialization_incremental_coordinator(request,
																   *make_warm_plan,
																   std::move(corrupt_bindings),
																   corrupt_executor,
																   producer,
																   guarantee);
		require(!corrupt && corrupt.error().code == "materialization.incremental-invalid" &&
					corrupt_executor.calls == 0U,
				"corrupt prior artifact was reused");

		prior = seal_all(request);
		auto stale_b =
			incremental_binding(request,
								"partition:b",
								1U,
								second,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])});
		stale_b.partitions.front().prior_artifact->state.input.source_digest =
			incremental_digest('f');
		std::vector<materialization_incremental_task_binding> stale_bindings;
		stale_bindings.emplace_back(std::move(stale_b));
		stale_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								first,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor stale_executor{
			request, producer, guarantee, false, false, false, 1U, false, std::move(prior)};
		auto stale = run_materialization_incremental_coordinator(request,
																 *make_warm_plan,
																 std::move(stale_bindings),
																 stale_executor,
																 producer,
																 guarantee);
		require(!stale && stale.error().code == "materialization.incremental-invalid" &&
					stale_executor.calls == 0U,
				"stale prior input was reused");

		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> receipt_bindings;
		receipt_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								unchanged,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		receipt_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								changed,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor zero_receipt_executor{
			request, producer, guarantee, false, false, false, 0U, false, std::move(prior)};
		auto zero_receipt = run_materialization_incremental_coordinator(request,
																		*affected_plan,
																		std::move(receipt_bindings),
																		zero_receipt_executor,
																		producer,
																		guarantee);
		require(!zero_receipt &&
					zero_receipt.error().code == "materialization.incremental-invalid" &&
					zero_receipt_executor.calls == 1U,
				"zero provider-call receipt was accepted");

		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> multi_receipt_bindings;
		multi_receipt_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								unchanged,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		multi_receipt_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								changed,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor multi_receipt_executor{
			request, producer, guarantee, false, false, false, 2U, false, std::move(prior)};
		auto multi_receipt =
			run_materialization_incremental_coordinator(request,
														*affected_plan,
														std::move(multi_receipt_bindings),
														multi_receipt_executor,
														producer,
														guarantee);
		require(!multi_receipt &&
					multi_receipt.error().code == "materialization.incremental-invalid" &&
					multi_receipt_executor.calls == 1U,
				"multiple provider-call receipt was accepted");

		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> streamless_bindings;
		streamless_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								unchanged,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		streamless_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								changed,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor streamless_executor{request,
														 producer,
														 guarantee,
														 false,
														 false,
														 false,
														 1U,
														 false,
														 std::move(prior),
														 0U,
														 true};
		auto streamless =
			run_materialization_incremental_coordinator(request,
														*affected_plan,
														std::move(streamless_bindings),
														streamless_executor,
														producer,
														guarantee);
		require(!streamless && streamless.error().code == "materialization.incremental-invalid" &&
					streamless.error().field == "ingress" && streamless_executor.calls == 1U,
				"coordinator bypassed the external event-stream ingress");

		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> pre_encoder_coverage_failure_bindings;
		pre_encoder_coverage_failure_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								unchanged,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		pre_encoder_coverage_failure_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								changed,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor pre_encoder_coverage_failure_executor{
			request,
			producer,
			guarantee,
			false,
			false,
			false,
			1U,
			false,
			std::move(prior),
			0U,
			false,
			coverage_mode::incomplete};
		auto pre_encoder_coverage_failure = run_materialization_incremental_coordinator(
			request,
			*affected_plan,
			std::move(pre_encoder_coverage_failure_bindings),
			pre_encoder_coverage_failure_executor,
			producer,
			guarantee);
		require(!pre_encoder_coverage_failure &&
					pre_encoder_coverage_failure.error().code ==
						"materialization.incremental-invalid" &&
					pre_encoder_coverage_failure.error().field == "receipt" &&
					pre_encoder_coverage_failure.error().detail ==
						"oracle-provider.coverage/canonical-balanced-covered" &&
					pre_encoder_coverage_failure_executor.calls == 1U &&
					pre_encoder_coverage_failure_executor.returned_executions == 1U,
				"incomplete coverage was not rejected by the pre-encoder oracle");

		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> mismatched_provider_receipt_bindings;
		mismatched_provider_receipt_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								unchanged,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		mismatched_provider_receipt_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								changed,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor mismatched_provider_receipt_executor{request,
																		  producer,
																		  guarantee,
																		  false,
																		  false,
																		  false,
																		  1U,
																		  false,
																		  std::move(prior),
																		  0U,
																		  false,
																		  coverage_mode::exact,
																		  true};
		auto mismatched_provider_receipt = run_materialization_incremental_coordinator(
			request,
			*affected_plan,
			std::move(mismatched_provider_receipt_bindings),
			mismatched_provider_receipt_executor,
			producer,
			guarantee);
		require(!mismatched_provider_receipt &&
					mismatched_provider_receipt.error().code ==
						"materialization.incremental-invalid" &&
					mismatched_provider_receipt.error().field == "receipt" &&
					mismatched_provider_receipt.error().detail == "sealed-transcript-mismatch" &&
					mismatched_provider_receipt_executor.calls == 1U,
				"provider sealed transcript receipt was not bound to the task result");

		prior = seal_all(request);
		std::vector<materialization_incremental_task_binding> publish_bindings;
		publish_bindings.emplace_back(
			incremental_binding(request,
								"partition:b",
								1U,
								unchanged,
								second,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(second, prior[1U])}));
		publish_bindings.emplace_back(
			incremental_binding(request,
								"partition:a",
								0U,
								changed,
								first,
								std::optional<materialization_incremental_prior_artifact>{
									incremental_prior_artifact(first, prior[0U])}));
		fixture_incremental_executor publish_executor{
			request, producer, guarantee, false, false, false, 1U, false, std::move(prior)};
		auto published =
			run_materialization_incremental_coordinator_and_publish(request,
																	*affected_plan,
																	std::move(publish_bindings),
																	publish_executor,
																	producer,
																	guarantee);
		require(published && published->store().publication_attempted &&
					published->publication_verified() &&
					published->store().publish_call_count == 1U &&
					published->store().publish_returned_record.has_value() &&
					published->store().verification_store.has_value() &&
					!published->store().first_issue.has_value(),
				"incremental coordinator did not retain a successful Store publication receipt");
	}

	void check_bounded_adoption_fail_closed(const validated_materialization_request& request,
											const materialization_producer_authority& producer)
	{
		const materialization_guarantee_authority guarantee{
			{}, {"clang22-parse", "query-parity", "store-reopen"}};
		const auto make_task = [&](const std::size_t task_index)
		{
			auto sealed = seal_task(request, task_index);
			require(sealed.has_value(), "bounded adoption fail-closed fixture seal failed");
			auto task = construct_materialization_bounded_task_claims(
				request, task_index, *sealed, producer, guarantee);
			require(task.has_value(),
					"bounded adoption fail-closed task construction failed: " +
						(task ? std::string{} : failure(task.error())));
			return std::move(*task);
		};

		auto incomplete_source = materialization_bounded_claim_source::begin(request);
		require(incomplete_source.has_value(), "incomplete bounded source begin failed");
		auto incomplete_task = make_task(0U);
		require(incomplete_source->consume_task(std::move(incomplete_task)).has_value(),
				"incomplete bounded source task adoption failed");
		auto incomplete_finalized = std::move(*incomplete_source).finalize();
		require(!incomplete_finalized && incomplete_finalized.error().field == "lifecycle" &&
					incomplete_finalized.error().detail == "incomplete-task-set",
				"bounded adoption finalized before consuming the declared task set");

		auto source = materialization_bounded_claim_source::begin(request);
		require(source.has_value(), "bounded adoption fail-closed source begin failed");
		auto metadata_drift = make_task(0U);
		metadata_drift.partitions.front().sdk_claim_occurrence_count += 1U;
		auto rejected = source->consume_task(std::move(metadata_drift));
		require(!rejected && rejected.error().field == "partition" &&
					rejected.error().detail == "claim-census" && source->partition_count() == 0U,
				"bounded adoption accepted metadata drift or mutated before validation");
		auto retry = make_task(0U);
		auto retry_result = source->consume_task(std::move(retry));
		require(!retry_result && retry_result.error().field == "lifecycle",
				"bounded adoption did not remain single-use after rejection");
		auto finalized_failed = std::move(*source).finalize();
		require(!finalized_failed && finalized_failed.error().field == "lifecycle",
				"failed bounded adoption source became finalizable");

		auto exact_source = materialization_bounded_claim_source::begin(request);
		require(exact_source.has_value(), "exact bounded source begin failed");
		require(exact_source->consume_task(make_task(0U)).has_value(),
				"exact bounded source first task adoption failed");
		require(exact_source->consume_task(make_task(1U)).has_value(),
				"exact bounded source second task adoption failed");
		auto exact_finalized = std::move(*exact_source).finalize();
		require(exact_finalized && exact_finalized->sealed(),
				"bounded source rejected the exact declared task census");
		auto exact_streaming_transaction =
			make_materialization_streaming_store_transaction(request, *exact_finalized);
		require(exact_streaming_transaction &&
					exact_finalized->task_count() == request.tasks.size(),
				"bounded Store metadata rejected the exact request/source binding");
		auto mismatched_request = request;
		mismatched_request.tasks.pop_back();
		auto mismatched_streaming_transaction =
			make_materialization_streaming_store_transaction(mismatched_request, *exact_finalized);
		require(!mismatched_streaming_transaction &&
					mismatched_streaming_transaction.error().code ==
						"materialization.task-binding-mismatch" &&
					mismatched_streaming_transaction.error().field == "store.source" &&
					mismatched_streaming_transaction.error().detail ==
						"request-identity-or-task-census",
				"bounded Store metadata accepted a source with a mismatched request task census");

		auto over_adoption_source = materialization_bounded_claim_source::begin(request);
		require(over_adoption_source.has_value(), "over-adoption bounded source begin failed");
		require(over_adoption_source->consume_task(make_task(0U)).has_value(),
				"over-adoption bounded source first task adoption failed");
		require(over_adoption_source->consume_task(make_task(1U)).has_value(),
				"over-adoption bounded source second task adoption failed");
		const auto partition_count_before_extra = over_adoption_source->partition_count();
		auto extra_task = over_adoption_source->consume_task(make_task(0U));
		require(!extra_task && extra_task.error().field == "lifecycle" &&
					extra_task.error().detail == "task-count" &&
					over_adoption_source->partition_count() == partition_count_before_extra,
				"bounded adoption accepted a task beyond the exact request census");
		auto over_adoption_finalized = std::move(*over_adoption_source).finalize();
		require(!over_adoption_finalized && over_adoption_finalized.error().field == "lifecycle",
				"over-adopting bounded source became finalizable");

		auto duplicate_source = materialization_bounded_claim_source::begin(request);
		require(duplicate_source.has_value(), "duplicate partition source begin failed");
		auto duplicate = make_task(0U);
		require(!duplicate.partitions.empty(), "duplicate partition fixture is empty");
		duplicate.partitions.push_back(duplicate.partitions.back());
		auto duplicate_result = duplicate_source->consume_task(std::move(duplicate));
		require(!duplicate_result && duplicate_result.error().field == "partitions" &&
					duplicate_result.error().detail == "noncanonical-order" &&
					duplicate_source->partition_count() == 0U,
				"bounded adoption accepted a duplicate partition window");

		auto out_of_order_source = materialization_bounded_claim_source::begin(request);
		require(out_of_order_source.has_value(), "out-of-order source begin failed");
		auto out_of_order = out_of_order_source->consume_task(make_task(1U));
		require(!out_of_order && out_of_order.error().field == "task-order" &&
					out_of_order.error().detail == "canonical-next" &&
					out_of_order_source->partition_count() == 0U,
				"bounded adoption staged an out-of-order task window");

		auto duplicate_task_source = materialization_bounded_claim_source::begin(request);
		require(duplicate_task_source.has_value(), "duplicate task source begin failed");
		require(duplicate_task_source->consume_task(make_task(0U)).has_value(),
				"duplicate task source first window adoption failed");
		const auto partition_count_before_duplicate = duplicate_task_source->partition_count();
		auto relabeled_task = make_task(0U);
		relabeled_task.canonical_task_index = 1U;
		auto relabeled_result = duplicate_task_source->consume_task(std::move(relabeled_task));
		require(!relabeled_result && relabeled_result.error().field == "task-binding" &&
					relabeled_result.error().detail == "sealed-index" &&
					duplicate_task_source->partition_count() == partition_count_before_duplicate,
				"bounded adoption accepted a relabeled duplicate task window");

		auto duplicate_task_source_after_relabel =
			materialization_bounded_claim_source::begin(request);
		require(duplicate_task_source_after_relabel.has_value(),
				"duplicate task source after relabel begin failed");
		require(duplicate_task_source_after_relabel->consume_task(make_task(0U)).has_value(),
				"duplicate task source after relabel first window adoption failed");
		const auto partition_count_before_duplicate_after_relabel =
			duplicate_task_source_after_relabel->partition_count();
		auto duplicate_task = duplicate_task_source_after_relabel->consume_task(make_task(0U));
		require(!duplicate_task && duplicate_task.error().field == "task-order" &&
					duplicate_task.error().detail == "canonical-next" &&
					duplicate_task_source_after_relabel->partition_count() ==
						partition_count_before_duplicate_after_relabel,
				"bounded adoption staged a duplicate task window");
	}
} // namespace

int main(const int argc, char** argv)
{
	static_assert(!std::is_copy_constructible_v<sealed_materialization_claims>);
	static_assert(!std::is_copy_assignable_v<sealed_materialization_claims>);
	static_assert(std::is_move_constructible_v<sealed_materialization_claims>);
	static_assert(std::is_move_assignable_v<sealed_materialization_claims>);
	const std::filesystem::path root = argc > 1 ? argv[1] : ".";
	auto request = request_fixture();
	auto producer = producer_authority(root);
	check_partition_event_stream();
	check_incremental_receipts(request);
	check_incremental_ingress(request);
	check_claim_stream_source(request);
	positive_and_zero_partitions(request, producer);
	streaming_source_receipts_replace_resident_payloads(root);
	check_incremental_coordinator_v2_1(root);
	check_incremental_coordinator(request, producer);
	check_bounded_adoption_fail_closed(request, producer);
	negative_authority_guarantee_order_and_coverage(request, std::move(producer));
}
