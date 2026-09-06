#include "application_analysis_run_command_service_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/sdk/query.hpp>

#include "application_analysis_command_service_internal.hpp"
#include "bounded_json_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		struct initial_relation_context
		{
			relation_engine engine;
			std::vector<std::string> relation_ids;
		};

		[[nodiscard]] error run_error(std::string field, std::string detail)
		{
			return {"application-analysis.run-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] result<void>
		add_string(json_value::object_type& object, std::string key, const std::string_view value)
		{
			auto encoded = json_value::string(std::string{value});
			if (!encoded)
				return unexpected(std::move(encoded.error()));
			object.emplace(std::move(key), std::move(*encoded));
			return {};
		}

		[[nodiscard]] std::string plain_digest(const std::string_view value)
		{
			return content_digest(std::as_bytes(std::span{value.data(), value.size()}));
		}

		[[nodiscard]] result<json_value> object(json_value::object_type value)
		{
			return json_value::object(std::move(value));
		}

		[[nodiscard]] std::string_view
		terminal_name(const materialization_terminal terminal) noexcept
		{
			switch (terminal)
			{
				case materialization_terminal::published_complete:
					return "published_complete";
				case materialization_terminal::published_partial:
					return "published_partial";
				case materialization_terminal::rejected:
					return "rejected";
				case materialization_terminal::failed:
					return "failed";
				case materialization_terminal::cancelled:
					return "cancelled";
			}
			return "failed";
		}

		[[nodiscard]] std::string_view
		query_execution_name(const query::execution_status status) noexcept
		{
			switch (status)
			{
				case query::execution_status::complete:
					return "complete";
				case query::execution_status::truncated:
					return "truncated";
				case query::execution_status::cancelled_with_partial:
					return "cancelled_with_partial";
				case query::execution_status::failed_before_result:
					return "failed_before_result";
			}
			return "failed_before_result";
		}

		[[nodiscard]] result<json_value>
		provider_unresolved(const std::span<const provider::unresolved_item> values)
		{
			json_value::array_type output;
			output.reserve(values.size());
			for (const auto& value : values)
			{
				json_value::object_type item;
				if (auto added = add_string(item, "code", value.code); !added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(item, "detail", value.detail); !added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(item, "subject", value.subject); !added)
					return unexpected(std::move(added.error()));
				auto encoded = object(std::move(item));
				if (!encoded)
					return unexpected(std::move(encoded.error()));
				output.push_back(std::move(*encoded));
			}
			return json_value::array(std::move(output));
		}

		[[nodiscard]] result<json_value>
		coverage_projection(const std::span<const provider::coverage_unit> values)
		{
			json_value::array_type output;
			output.reserve(values.size());
			for (const auto& value : values)
			{
				json_value::object_type item;
				if (auto added = add_string(item, "id", value.id); !added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(item, "kind", value.kind); !added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(item, "reason", value.reason); !added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(item, "state", value.state); !added)
					return unexpected(std::move(added.error()));
				auto encoded = object(std::move(item));
				if (!encoded)
					return unexpected(std::move(encoded.error()));
				output.push_back(std::move(*encoded));
			}
			return json_value::array(std::move(output));
		}

		[[nodiscard]] result<json_value>
		query_unresolved(const std::span<const query::query_unresolved> values)
		{
			json_value::array_type output;
			output.reserve(values.size());
			for (const auto& value : values)
			{
				json_value::object_type item;
				if (auto added = add_string(item, "code", value.code); !added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(item, "detail", value.detail); !added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(item, "subject", value.subject); !added)
					return unexpected(std::move(added.error()));
				auto encoded = object(std::move(item));
				if (!encoded)
					return unexpected(std::move(encoded.error()));
				output.push_back(std::move(*encoded));
			}
			return json_value::array(std::move(output));
		}

		[[nodiscard]] result<initial_relation_context> make_initial_relation_context()
		{
			const std::array descriptors{
				&build::relations::project::descriptor(),
				&build::relations::compile_unit::descriptor(),
				&build::relations::variant::descriptor(),
				&build::relations::toolchain_context::descriptor(),
				&source::relations::file::descriptor(),
				&source::relations::span::descriptor(),
				&cc::relations::entity::descriptor(),
				&cc::relations::declaration::descriptor(),
				&cc::relations::type::descriptor(),
				&cc::relations::call_site::descriptor(),
				&cc::relations::call_direct_target::descriptor(),
			};
			relation_registry registry;
			std::vector<std::string> relation_ids;
			relation_ids.reserve(descriptors.size());
			for (const auto* descriptor : descriptors)
			{
				if (auto added = registry.add(*descriptor); !added)
					return unexpected(std::move(added.error()));
				relation_ids.push_back(descriptor->id);
			}
			std::ranges::sort(relation_ids);
			auto engine = registry.build("application-analysis-initial-v1");
			if (!engine)
				return unexpected(std::move(engine.error()));
			return initial_relation_context{std::move(*engine), std::move(relation_ids)};
		}

		[[nodiscard]] result<provider::provider_candidate>
		make_worker_candidate(const application_analysis_run_command_request& request,
							  const std::vector<std::string>& relation_ids)
		{
			if (request.worker_path.empty() || request.worker_path.front() != '/' ||
				request.worker_path.contains('\0'))
				return unexpected(run_error("worker", "absolute-path-required"));
			auto semantics = plain_digest("cxxlens.clang23-gcc-replay-provider.v1\n"
										  "clang-23.1.0-gcc-mode");
			auto invalidation = plain_digest("cxxlens.clang23-gcc-replay-invalidation.v1\n"
											 "capture-and-replay-plan-digests");
			auto determinism = plain_digest("cxxlens.clang23-gcc-replay-determinism.v1\n"
											"canonical-observation-normalization");
			auto policies = provider::builtin_sandbox_policies();
			if (policies.empty())
				return unexpected(run_error("sandbox", "policy-unavailable"));
			const auto& policy = policies.front();
			auto evidence = provider::sandbox_evidence_digest(policy,
															  request.budget,
															  provider::sandbox_assurance::enforced,
															  policy.mechanisms,
															  request.trusted_worker_digest);
			if (!evidence)
				return unexpected(std::move(evidence.error()));

			provider::manifest manifest;
			manifest.provider_id = "cxxlens.clang23-gcc-replay";
			manifest.provider_version = {1U, 0U, 0U};
			manifest.package_identity = "cxxlens.clang23-gcc-replay.package";
			manifest.publisher = "cxxlens.project";
			manifest.license = "Apache-2.0 WITH LLVM-exception";
			manifest.protocol = {provider::protocol_v2_major,
								 provider::protocol_v2_minor,
								 provider::protocol_v2_minor,
								 {"credit-backpressure", "task-input-chunks-v2"},
								 {}};
			manifest.platform_tuples = {"linux-x86_64-clang23"};
			manifest.provider_binary_digest = request.trusted_worker_digest;
			manifest.provider_semantic_contract_digest = std::move(semantics);
			manifest.offered_relations = relation_ids;
			manifest.interpretation_domains = {"cc.clang23-gcc-replay-1"};
			manifest.invalidation_contract = std::move(invalidation);
			manifest.determinism_contract = std::move(determinism);
			manifest.resource_class = "provider.application-analysis";
			manifest.requested_qualifications = {"experimental"};
			if (auto valid = manifest.validate(); !valid)
				return unexpected(std::move(valid.error()));
			return provider::provider_candidate{std::move(manifest),
												provider::discovery_source::explicit_path,
												{request.worker_path},
												true,
												true,
												true,
												{"experimental"},
												{"linux-glibc",
												 policy.mechanisms,
												 provider::sandbox_assurance::enforced,
												 policy.policy_digest(),
												 std::move(*evidence)},
												{}};
		}

		[[nodiscard]] result<snapshot_draft>
		make_publication(const initial_relation_context& context,
						 const imported_project& project,
						 const std::string_view trusted_worker_digest)
		{
			auto interpretation = semantic_digest("cxxlens.application-analysis-interpretation.v1",
												  "cc.clang23-gcc-replay-1");
			auto trust = semantic_digest("cxxlens.application-analysis-explicit-worker-trust.v1",
										 trusted_worker_digest);
			if (!interpretation || !trust)
				return unexpected(run_error("publication", "identity-derivation"));
			return snapshot_draft{{std::string{project.id()},
								   "experimental",
								   std::string{context.engine.generation()},
								   "condition:application-analysis-initial-v1",
								   std::string{context.engine.registry_digest()},
								   std::move(*interpretation),
								   std::move(*trust)},
								  {1U, 0U, 0U},
								  std::string{project.catalog_semantic_digest()},
								  std::nullopt};
		}

		[[nodiscard]] result<json_value>
		query_projection(const relation_engine& engine,
						 const snapshot_handle& snapshot,
						 const std::vector<std::string>& relation_ids)
		{
			auto runtime = query::reference_engine::bind(snapshot);
			if (!runtime)
				return unexpected(std::move(runtime.error()));
			json_value::array_type output;
			output.reserve(relation_ids.size());
			for (const auto& relation_id : relation_ids)
			{
				auto descriptor = engine.require_id(relation_id);
				if (!descriptor)
					return unexpected(std::move(descriptor.error()));
				auto logical = query::builder::from(descriptor->descriptor());
				if (!logical)
					return unexpected(std::move(logical.error()));
				auto queried = runtime->execute(std::move(*logical).finish());
				if (!queried)
					return unexpected(std::move(queried.error()));
				std::uint64_t row_count{};
				auto rows = queried->rows();
				for (;;)
				{
					auto next = rows.next();
					if (!next)
						return unexpected(std::move(next.error()));
					if (!*next)
						break;
					if (row_count == std::numeric_limits<std::uint64_t>::max())
						return unexpected(run_error("query", "row-count-overflow"));
					++row_count;
				}
				json_value::object_type item;
				if (auto added =
						add_string(item, "execution", query_execution_name(queried->execution()));
					!added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(item, "relation_id", relation_id); !added)
					return unexpected(std::move(added.error()));
				item.emplace("closed", json_value::boolean(queried->closed()));
				item.emplace("inputs_complete", json_value::boolean(queried->inputs_complete()));
				item.emplace("row_count", json_value::unsigned_integer(row_count));
				auto unresolved = query_unresolved(queried->unresolved_items());
				if (!unresolved)
					return unexpected(std::move(unresolved.error()));
				item.emplace("unresolved", std::move(*unresolved));
				auto encoded = object(std::move(item));
				if (!encoded)
					return unexpected(std::move(encoded.error()));
				output.push_back(std::move(*encoded));
			}
			return json_value::array(std::move(output));
		}

		[[nodiscard]] result<std::string>
		result_projection(const materialization_result& result,
						  const relation_engine& engine,
						  const std::vector<std::string>& relation_ids)
		{
			json_value::object_type materialization;
			if (auto added =
					add_string(materialization, "terminal", terminal_name(result.terminal()));
				!added)
				return unexpected(std::move(added.error()));
			materialization.emplace("conflict_count",
									json_value::unsigned_integer(result.conflicts().size()));
			materialization.emplace(
				"differential_disagreement_count",
				json_value::unsigned_integer(result.differential_disagreements().size()));
			auto coverage = coverage_projection(result.coverage());
			if (!coverage)
				return unexpected(std::move(coverage.error()));
			materialization.emplace("coverage", std::move(*coverage));
			auto unresolved = provider_unresolved(result.unresolved());
			if (!unresolved)
				return unexpected(std::move(unresolved.error()));
			materialization.emplace("unresolved", std::move(*unresolved));
			if (result.published_snapshot())
			{
				if (auto added = add_string(
						materialization, "snapshot_id", result.published_snapshot()->id());
					!added)
					return unexpected(std::move(added.error()));
			}
			else
				materialization.emplace("snapshot_id", json_value::null());

			if (result.provenance())
			{
				json_value::object_type provenance;
				if (auto added =
						add_string(provenance, "provider_id", result.provenance()->provider_id);
					!added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(
						provenance,
						"provider_version",
						std::to_string(result.provenance()->provider_version.major) + "." +
							std::to_string(result.provenance()->provider_version.minor) + "." +
							std::to_string(result.provenance()->provider_version.patch));
					!added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(provenance,
											"provider_binary_digest",
											result.provenance()->provider_binary_digest);
					!added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(provenance,
											"provider_semantics_digest",
											result.provenance()->provider_semantics_digest);
					!added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(
						provenance, "replay_plan_digest", result.provenance()->replay_plan_digest);
					!added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(provenance,
											"runtime_receipt_digest",
											result.provenance()->runtime_receipt_digest);
					!added)
					return unexpected(std::move(added.error()));
				if (auto added = add_string(
						provenance, "task_input_digest", result.provenance()->task_input_digest);
					!added)
					return unexpected(std::move(added.error()));
				auto encoded = object(std::move(provenance));
				if (!encoded)
					return unexpected(std::move(encoded.error()));
				materialization.emplace("provenance", std::move(*encoded));
			}
			else
				materialization.emplace("provenance", json_value::null());

			auto materialization_object = object(std::move(materialization));
			if (!materialization_object)
				return unexpected(std::move(materialization_object.error()));
			json_value::object_type root;
			root.emplace("materialization", std::move(*materialization_object));
			if (result.published_snapshot())
			{
				auto queries = query_projection(engine, *result.published_snapshot(), relation_ids);
				if (!queries)
					return unexpected(std::move(queries.error()));
				root.emplace("queries", std::move(*queries));
			}
			else
				root.emplace("queries", json_value::array({}));
			auto root_object = object(std::move(root));
			if (!root_object)
				return unexpected(std::move(root_object.error()));
			return canonical_json_line(*root_object);
		}
	} // namespace

	result<application_analysis_run_command_result>
	run_application_analysis_command(const application_analysis_run_command_request& request)
	{
		try
		{
			auto loaded = load_application_analysis({request.bundle_path, request.limits});
			if (!loaded)
				return unexpected(std::move(loaded.error()));
			if (loaded->bundle.production_compiler() != "gcc-16.2.0" ||
				loaded->bundle.target_abi() != "x86_64-linux-gnu")
				return unexpected(run_error("bundle", "unsupported-toolchain"));
			auto context = make_initial_relation_context();
			if (!context)
				return unexpected(std::move(context.error()));
			auto candidate = make_worker_candidate(request, context->relation_ids);
			if (!candidate)
				return unexpected(std::move(candidate.error()));
			auto publication =
				make_publication(*context, loaded->project, request.trusted_worker_digest);
			if (!publication)
				return unexpected(std::move(publication.error()));
			provider::provider_selection_request selection{
				candidate->description.provider_id,
				candidate->description.provider_version,
				candidate->description.provider_binary_digest,
				candidate->description.provider_semantic_contract_digest,
				{provider::sandbox_assurance::enforced, candidate->sandbox.policy_digest},
				true,
				std::nullopt};
			auto materialization = materialization_request::make(context->engine,
																 std::move(*publication),
																 context->relation_ids,
																 "cc.clang23-gcc-replay-1",
																 std::move(selection),
																 {std::move(*candidate)},
																 request.budget);
			if (!materialization)
				return unexpected(std::move(materialization.error()));
			auto store = make_in_memory_snapshot_store(context->engine);
			if (!store)
				return unexpected(std::move(store.error()));
			auto analyzed = materialize(*store, loaded->project, *materialization);
			if (!analyzed)
				return unexpected(std::move(analyzed.error()));
			auto encoded = result_projection(*analyzed, context->engine, context->relation_ids);
			if (!encoded)
				return unexpected(std::move(encoded.error()));
			return application_analysis_run_command_result{analyzed->terminal(),
														   std::move(*encoded)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(run_error("memory", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(run_error("memory", "allocation-length"));
		}
	}
} // namespace cxxlens::sdk::detail
