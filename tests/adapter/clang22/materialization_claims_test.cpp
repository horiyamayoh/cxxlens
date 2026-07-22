#include "llvm/clang22/materialization_claims.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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

#include "llvm/clang22/materialization_pipeline.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;
	using namespace cxxlens::detail::clang22::materialization;

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

	[[nodiscard]] validated_materialization_request request_fixture()
	{
		constexpr std::string_view request_json =
			R"cxxlens_json({"engine":{"admitted_descriptors":[{"descriptor_id":"build.compile_unit.v1","runtime_descriptor_digest":"semantic-v2:sha256:1dde734221f3db42a0bdadd531740c35e6f30c15fe196e0b20e1b60c2cf54679"},{"descriptor_id":"build.project.v1","runtime_descriptor_digest":"semantic-v2:sha256:97e5d3d4546803be5de464e5d5de7617b9f4ed29bcb81e503dc6c5a613277cd9"},{"descriptor_id":"build.toolchain_context.v1","runtime_descriptor_digest":"semantic-v2:sha256:3e8895ed57aca936310888a256c4ed31911b46fe5bbac5e045a80f80801cc4e0"},{"descriptor_id":"build.variant.v1","runtime_descriptor_digest":"semantic-v2:sha256:56c59d76bd7921d01c54118470d2643eee5ff8e4ed0ce275f69e9d6ef45500e6"},{"descriptor_id":"cc.call_direct_target.v1","runtime_descriptor_digest":"semantic-v2:sha256:888196009a7344c3cfb198c0c01a359f49e4f042b998d34efc4057c3ba4e56d4"},{"descriptor_id":"cc.call_site.v1","runtime_descriptor_digest":"semantic-v2:sha256:8377b659e3703eef0acb446ab6b07e94aa4655aba33aa5b430e5cf65491163f2"},{"descriptor_id":"cc.entity.v1","runtime_descriptor_digest":"semantic-v2:sha256:4537eb3f074379aa8c2222c9d2ed5dc530340bf1b2b5c862b4cf52b0c37b1b3e"},{"descriptor_id":"frontend.clang22.call_observation.v2","runtime_descriptor_digest":"semantic-v2:sha256:8b79a9fb3d59e750c51310d6f32935701a36c68fd5830228516482b0e7d2cd65"},{"descriptor_id":"frontend.clang22.entity_observation.v2","runtime_descriptor_digest":"semantic-v2:sha256:eb909eec97cec22586f4ac67dc7c56cc29390857df9355186feae5e9ce7700fb"},{"descriptor_id":"frontend.clang22.type_observation.v2","runtime_descriptor_digest":"semantic-v2:sha256:94b6f6efcd46dad74c0cec1c761a2d363c6acdfe135862c37d0b7e28b01b6026"},{"descriptor_id":"source.file.v1","runtime_descriptor_digest":"semantic-v2:sha256:3aebbb05303ba924f1c25547242a656c59d95c265fe99cc3fd77db8633af8609"},{"descriptor_id":"source.span.v1","runtime_descriptor_digest":"semantic-v2:sha256:055e5a6997fef2d1c2dcebfe10baa41813c0ccec091409ad84a1081fd8894a86"}],"engine_generation_id":"engine-generation:sha256:984ec980908d8a3e3d14fb81b06e06009249e909bc7a6d323b447de825da08eb","engine_registry_digest":"semantic-v2:sha256:051823ea2f538bf38656afefb81d22950e5a6ca671fa4d57d89fffd8cfba171a","generation_contract":"cxxlens.clang22-materialization-engine.v2"},"group_topology":{"atomic_output_group":"clang22-atomic","dependency_groups":["canonical","observation"],"partial_policy":"forbid"},"interpretation_policy":{"interpretation_policy_digest":"semantic-v2:sha256:3e97b2cb497e80e0f59953844b4050930e3919f36ac3aab7403d391ab4cc087f","policy_id":"cxxlens.clang22-interpretation-policy.v1","selected_domain":"cc.clang22-canonical-1"},"materialization_request_id":"materialization:semantic-v2:sha256:09a36429bc4dc0f74ef0bf23a6751837d8b0277c06392c9ac5e64c9dab66f95a","project":{"catalog_compile_unit_census_digest":"semantic-v2:sha256:806e8f7964f77dcec9a30078129430a733c89e39488e7fae80b68c7a50d186ba","catalog_compile_units":[{"catalog_compile_unit_id":"catalog-unit:0000","effective_invocation_digest":"semantic-v2:sha256:dd5bdb2f9fd85376546c2f486a1ac3ebeed4bdb922351f3c2f4a7bf89be94acb","environment_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","source_digest":"sha256:deac66ccb79f6d31c0fa7d358de48e083c15c02ff50ec1ebd4b64314b9e6e196"},{"catalog_compile_unit_id":"catalog-unit:0001","effective_invocation_digest":"semantic-v2:sha256:68f779154ad8159b42f2ccc79b7e74999742e0c05a05563421e50d0cae028c09","environment_digest":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","source_digest":"sha256:76af64be58a1f67608cb4c34771305ad773b173cc4cde76261d749928ad4ea49"}],"catalog_digest":"semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","catalog_environment_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","logical_root":"project://fixture","project_id":"project:sha256:9b8cdb2a5afab245af006c61b1bbf0a758687ed969b42d349caf98bcdb6f01c3"},"publication":{"backend":"memory","expected_parent_publication":null,"genesis":true,"partial_policy":"forbid","reopen_before_success":true,"selector":{"catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","channel_id":"channel:clang22-production","condition_universe_id":"condition-universe:one","engine_generation_id":"engine-generation:sha256:984ec980908d8a3e3d14fb81b06e06009249e909bc7a6d323b447de825da08eb","interpretation_policy_digest":"semantic-v2:sha256:3e97b2cb497e80e0f59953844b4050930e3919f36ac3aab7403d391ab4cc087f","relation_registry_digest":"semantic-v2:sha256:051823ea2f538bf38656afefb81d22950e5a6ca671fa4d57d89fffd8cfba171a","trust_policy_digest":"semantic-v2:sha256:a0b190b934d43470d18cbbf326601174fe8a23e52e825c904b8d265dc990d053"},"series_id":"snapshot-series:sha256:c405319c06ab507d9ec7bff97664b5ddac4d211549f4cd72f4fc56621666cdd3","sqlite_path":null,"transaction_count":1},"registry":{"authority_registry_digest":"sha256:4caf626ec6f198118802f22d9cac62b02b2c3bb392fdc8d68b1a58f8101c342e","base_descriptors":[{"contract_digest":"sha256:a0b4b380ab0f5b631fa8ff59c39dcfbd859e26f849d169ae5d6a428e2f9eff5f","descriptor_id":"build.project.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:97e5d3d4546803be5de464e5d5de7617b9f4ed29bcb81e503dc6c5a613277cd9","stage_order":0},{"contract_digest":"sha256:06383e29854c5ce463c996a7a36b6954a4d6388b8384ddc39ad62688bdac0663","descriptor_id":"build.toolchain_context.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:3e8895ed57aca936310888a256c4ed31911b46fe5bbac5e045a80f80801cc4e0","stage_order":1},{"contract_digest":"sha256:1594c6f7ee0f80fdb59f11a9ab45a9521a8aab889052ba3fa40cf1d790aa66a1","descriptor_id":"build.variant.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:56c59d76bd7921d01c54118470d2643eee5ff8e4ed0ce275f69e9d6ef45500e6","stage_order":2},{"contract_digest":"sha256:3c325526160c00ceccd0c43f384689fff95187ef97f926871917ce6b4f7f429a","descriptor_id":"source.file.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:3aebbb05303ba924f1c25547242a656c59d95c265fe99cc3fd77db8633af8609","stage_order":3},{"contract_digest":"sha256:8b019f86c953ce3d08475a726b16dcb355e1474238b6a4300d7dd3dc9fc299b3","descriptor_id":"build.compile_unit.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:1dde734221f3db42a0bdadd531740c35e6f30c15fe196e0b20e1b60c2cf54679","stage_order":4},{"contract_digest":"sha256:645a46ad50ee0c84276ff4e09b2818486bfafe8c631f66368d45aa47cbe659ff","descriptor_id":"source.span.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:055e5a6997fef2d1c2dcebfe10baa41813c0ccec091409ad84a1081fd8894a86","stage_order":5}],"descriptors":[{"atomic_output_group_id":"clang22-atomic","batch_id":"cc.call_direct_target.v1-batch","contract_digest":"sha256:e2960ef9dff7a1190aa6b687281e0b1aeaddfcc684f35a9870323d5716697b2b","dependency_group_id":"canonical","descriptor_id":"cc.call_direct_target.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","runtime_descriptor_digest":"semantic-v2:sha256:888196009a7344c3cfb198c0c01a359f49e4f042b998d34efc4057c3ba4e56d4"},{"atomic_output_group_id":"clang22-atomic","batch_id":"cc.call_site.v1-batch","contract_digest":"sha256:4b8f7b76ef8087485462762bfef006e3fad50354da2738a61402441e9e53510e","dependency_group_id":"canonical","descriptor_id":"cc.call_site.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","runtime_descriptor_digest":"semantic-v2:sha256:8377b659e3703eef0acb446ab6b07e94aa4655aba33aa5b430e5cf65491163f2"},{"atomic_output_group_id":"clang22-atomic","batch_id":"cc.entity.v1-batch","contract_digest":"sha256:89813f031dbe91daed64d5c9d3fa1aef22a1ddcf74cf00a29f292971541f9020","dependency_group_id":"canonical","descriptor_id":"cc.entity.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","runtime_descriptor_digest":"semantic-v2:sha256:4537eb3f074379aa8c2222c9d2ed5dc530340bf1b2b5c862b4cf52b0c37b1b3e"},{"atomic_output_group_id":"clang22-atomic","batch_id":"frontend.clang22.call_observation.v2-batch","contract_digest":"sha256:07ea48a7f00e80972ba59c14ee96f916772ad9ed57fc84e313e3958f08fa548a","dependency_group_id":"observation","descriptor_id":"frontend.clang22.call_observation.v2","descriptor_version":"2.0.0","output_stage":"assertion","runtime_descriptor_digest":"semantic-v2:sha256:8b79a9fb3d59e750c51310d6f32935701a36c68fd5830228516482b0e7d2cd65"},{"atomic_output_group_id":"clang22-atomic","batch_id":"frontend.clang22.entity_observation.v2-batch","contract_digest":"sha256:4a5012801fcde26110a9f6350177d74d7d6975edde96337d4d3918ca7a004d51","dependency_group_id":"observation","descriptor_id":"frontend.clang22.entity_observation.v2","descriptor_version":"2.0.0","output_stage":"assertion","runtime_descriptor_digest":"semantic-v2:sha256:eb909eec97cec22586f4ac67dc7c56cc29390857df9355186feae5e9ce7700fb"},{"atomic_output_group_id":"clang22-atomic","batch_id":"frontend.clang22.type_observation.v2-batch","contract_digest":"sha256:53c54f967eb041e75ea98463c212d259fed0d3a310038ac9c93209749e72387f","dependency_group_id":"observation","descriptor_id":"frontend.clang22.type_observation.v2","descriptor_version":"2.0.0","output_stage":"assertion","runtime_descriptor_digest":"semantic-v2:sha256:94b6f6efcd46dad74c0cec1c761a2d363c6acdfe135862c37d0b7e28b01b6026"}],"path":"schemas/cxxlens_ng_relation_registry.yaml"},"request_digest":"semantic-v2:sha256:09a36429bc4dc0f74ef0bf23a6751837d8b0277c06392c9ac5e64c9dab66f95a","request_version":"2.0.0","schema":"cxxlens.clang22-materialization-request.v2","semantic_request_digest":"semantic-v2:sha256:7d79bd07fade21afe4701e0b55814701792b4b285ab823282e70e229c82e0bdd","tasks":[{"budget":{"address_space_bytes":1073741824,"cpu_ms":10000,"diagnostics":128,"open_files":64,"output_bytes":1048576,"rows":1024,"subprocesses":1,"transport_bytes":2097152,"wall_ms":10000},"build_variant_id":"build-variant:sha256:d0d2c433d8c558923be73e7655f2faa65ea94e330c9aa722d0e7d831d6907e01","catalog_digest":"semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","compile_unit_id":"compile-unit:sha256:be42bfee446b271dd490ce3477e4c2f74e8a6125a6f3ec8a03bbcbe349161e99","condition_id":"condition:all","condition_universe_id":"condition-universe:one","dependency_groups":["canonical","observation"],"effective_argv":["clang++","-std=c++23","project://main.cpp"],"environment_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","interpretation_domain":"cc.clang22-canonical-1","language":"cxx","normalized_invocation_digest":"semantic-v2:sha256:dd5bdb2f9fd85376546c2f486a1ac3ebeed4bdb922351f3c2f4a7bf89be94acb","project_id":"project:sha256:9b8cdb2a5afab245af006c61b1bbf0a758687ed969b42d349caf98bcdb6f01c3","provider_execution_id":"provider-execution:sha256:b7a7f77301033ac30084e3fa657eac3914344e6ab26f3fdc6b52272e10d4c0b3","provider_task_id":"task:semantic-v2:sha256:5fb6b47f3aec5abedd658b2acd86f9a9e3af418712ed9fbf80e30cb3c7306118","requested_descriptor_ids":["cc.call_direct_target.v1","cc.call_site.v1","cc.entity.v1","frontend.clang22.call_observation.v2","frontend.clang22.entity_observation.v2","frontend.clang22.type_observation.v2"],"sandbox":{"minimum":"enforced","policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"selected_catalog_compile_unit_id":"catalog-unit:0000","source":{"content_base64":"aW50IG1haW4oKSB7IHJldHVybiAwOyB9Cg==","content_digest":"sha256:deac66ccb79f6d31c0fa7d358de48e083c15c02ff50ec1ebd4b64314b9e6e196","encoding":"utf8","file_id":"file:sha256:83e065cbf0d8f742fe73a01155b02057c0de0fbe747f88b35ea5e96efe8faf06","line_index_id":"line-index:sha256:99cec457c4ced432a4db1dbb3c30bc291044469abf025737b306ccc7980a3510","logical_path":"project://main.cpp","read_only":false,"size_bytes":25,"source_snapshot_id":"source-snapshot:sha256:cb28d4d99af02e2bf0d1efc7288f211f595ef0a0caeaed889b66cb0fe995086d"},"task_input_digest":"sha256:39bd328764ad9f47c49e0efdfee4d232c410dfa79f096bbed16ea6ca02fd8056","toolchain":{"abi_digest":"sha256:4444444444444444444444444444444444444444444444444444444444444444","builtin_headers_digest":"sha256:3333333333333333333333333333333333333333333333333333333333333333","exact_version":"22.0.0","family":"clang","plugin_spec_digest":"sha256:5555555555555555555555555555555555555555555555555555555555555555","sysroot":null,"target_triple":"x86_64-unknown-linux-gnu"},"toolchain_context_id":"toolchain-context:sha256:78f64803fb0f0f1ab7f10321ebc90aa52aafb99705407b92e005ff7d6ae82b9a","toolchain_digest":"semantic-v2:sha256:d84b82c787577126d2fbbc4e19f1608f77d1725216cf7647c6ace444d1917dbb","variant":{"include_search_digest":"sha256:7777777777777777777777777777777777777777777777777777777777777777","language":"cxx","language_standard":"cxx23","predefined_macros_digest":"sha256:6666666666666666666666666666666666666666666666666666666666666666","semantic_flags_digest":"sha256:8888888888888888888888888888888888888888888888888888888888888888","target_triple":"x86_64-unknown-linux-gnu"},"working_directory":"project://fixture"},{"budget":{"address_space_bytes":1073741824,"cpu_ms":10000,"diagnostics":128,"open_files":64,"output_bytes":1048576,"rows":1024,"subprocesses":1,"transport_bytes":2097152,"wall_ms":10000},"build_variant_id":"build-variant:sha256:d0d2c433d8c558923be73e7655f2faa65ea94e330c9aa722d0e7d831d6907e01","catalog_digest":"semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","compile_unit_id":"compile-unit:sha256:3c5db06dbb85f42d2c2d89246ccf445078097e49250efb894a0270ad2b0cd553","condition_id":"condition:all","condition_universe_id":"condition-universe:one","dependency_groups":["canonical","observation"],"effective_argv":["clang++","-std=c++23","project://unit_1.cpp"],"environment_digest":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","interpretation_domain":"cc.clang22-canonical-1","language":"cxx","normalized_invocation_digest":"semantic-v2:sha256:68f779154ad8159b42f2ccc79b7e74999742e0c05a05563421e50d0cae028c09","project_id":"project:sha256:9b8cdb2a5afab245af006c61b1bbf0a758687ed969b42d349caf98bcdb6f01c3","provider_execution_id":"provider-execution:sha256:8bab585913137bb9106c76c4e46e5934aa07161cffc37784592af415fd7eb784","provider_task_id":"task:semantic-v2:sha256:5fb6b47f3aec5abedd658b2acd86f9a9e3af418712ed9fbf80e30cb3c7306118","requested_descriptor_ids":["cc.call_direct_target.v1","cc.call_site.v1","cc.entity.v1","frontend.clang22.call_observation.v2","frontend.clang22.entity_observation.v2","frontend.clang22.type_observation.v2"],"sandbox":{"minimum":"enforced","policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"selected_catalog_compile_unit_id":"catalog-unit:0001","source":{"content_base64":"aW50IHVuaXRfMSgpIHsgcmV0dXJuIDE7IH0K","content_digest":"sha256:76af64be58a1f67608cb4c34771305ad773b173cc4cde76261d749928ad4ea49","encoding":"utf8","file_id":"file:sha256:c07309e8ee43ccbf4412cd0bbbb99099df12cd1e1a89ac84b7093308ac760b71","line_index_id":"line-index:sha256:5fe3bf322112a1740a8a1e95ed148bc1d8db4ad217b3574d6540c2de296da3a3","logical_path":"project://unit_1.cpp","read_only":false,"size_bytes":27,"source_snapshot_id":"source-snapshot:sha256:3663a8dd373452f9a641715395076673985bd5f2897e7b95983d0888464f9a93"},"task_input_digest":"sha256:6f097c492800c8a785ce8b69ff61ce8c106ab80b296be12c0ae6b92d3017b650","toolchain":{"abi_digest":"sha256:4444444444444444444444444444444444444444444444444444444444444444","builtin_headers_digest":"sha256:3333333333333333333333333333333333333333333333333333333333333333","exact_version":"22.0.0","family":"clang","plugin_spec_digest":"sha256:5555555555555555555555555555555555555555555555555555555555555555","sysroot":null,"target_triple":"x86_64-unknown-linux-gnu"},"toolchain_context_id":"toolchain-context:sha256:78f64803fb0f0f1ab7f10321ebc90aa52aafb99705407b92e005ff7d6ae82b9a","toolchain_digest":"semantic-v2:sha256:d84b82c787577126d2fbbc4e19f1608f77d1725216cf7647c6ace444d1917dbb","variant":{"include_search_digest":"sha256:7777777777777777777777777777777777777777777777777777777777777777","language":"cxx","language_standard":"cxx23","predefined_macros_digest":"sha256:6666666666666666666666666666666666666666666666666666666666666666","semantic_flags_digest":"sha256:8888888888888888888888888888888888888888888888888888888888888888","target_triple":"x86_64-unknown-linux-gnu"},"working_directory":"project://fixture"}],"tool":{"distribution_version":"1.0.0","executable":"cxxlens-clang22-materialize","installed_executable_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","interface_version":"2.0.0","package_configuration":"static","prefix_manifest_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","relocated_prefix_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","source_revision":"1111111111111111111111111111111111111111","source_tree":"2222222222222222222222222222222222222222"},"trust_policy":{"execution_profile":"trust.native-worker","policy_id":"cxxlens.clang22-installed-native-worker-trust.v1","protocol_major":1,"protocol_minor":0,"provider_id":"cxxlens.clang22.reference","provider_version":"1.0.0","required_qualification":"canonical-semantic-qualified","semantic_contract_digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","task_sandbox_requirements":[{"minimum":"enforced","policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}],"trust_policy_digest":"semantic-v2:sha256:a0b190b934d43470d18cbbf326601174fe8a23e52e825c904b8d265dc990d053","worker_sandbox_policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"worker":{"executable":"cxxlens-clang-worker-22","installed_binary_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","protocol_major":1,"protocol_minor":0,"provider_id":"cxxlens.clang22.reference","provider_version":"1.0.0","sandbox_policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","semantic_contract_digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})cxxlens_json";
		auto document = parse_json_object(std::string{request_json});
		require(document.has_value(),
				"request JSON parse failed: " +
					(document ? std::string{} : failure(document.error())));
		auto request = validate_materialization_request(std::move(*document));
		require(request.has_value(),
				"request fixture failed: " + (request ? std::string{} : failure(request.error())));
		return std::move(*request);
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
						"aa823aa09a61cde47abe743bb06eceb48af765f5fe81cc958725ed3ec03d34bd" &&
					claims->direct_basis_digest() ==
						"semantic-v2:sha256:"
						"ffec670faed3a2978465121a1e8d440f4856dbaa788f8446cc8dc10ab6030e2b" &&
					claims->canonical_adoption_transform_digest() ==
						"semantic-v2:sha256:"
						"c9bf1439f0019aa61e968d06dd31ab7e9dfb3ab15000e44c1be8c280c1ed72e0" &&
					claims->base_ingestion_transform_digest() ==
						"semantic-v2:sha256:"
						"0997e34a5040951e94b5ee21087d86d253b6579f3d4cab4c60082be4f60a0abb" &&
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
	positive_and_zero_partitions(request, producer);
	negative_authority_guarantee_order_and_coverage(request, std::move(producer));
}
