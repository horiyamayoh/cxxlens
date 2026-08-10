#include "materialization_public_report.hpp"

#include <array>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		using object = json_value::object_type;

		[[nodiscard]] sdk::error report_error(public_materialization_report_error error)
		{
			std::string detail = std::move(error.detail);
			if (!error.missing_fields.empty())
			{
				if (!detail.empty())
					detail += ':';
				detail += "missing=";
				for (std::size_t index{}; index < error.missing_fields.size(); ++index)
				{
					if (index != 0U)
						detail += ',';
					detail += error.missing_fields[index];
				}
			}
			return {"materialization.report-invalid", std::move(error.field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<json_value> string(std::string value)
		{
			return json_value::string(std::move(value));
		}

		[[nodiscard]] sdk::result<json_value> make_object(object value)
		{
			auto result = json_value::object(std::move(value));
			if (!result)
				return sdk::unexpected(
					{"materialization.report-invalid", "json.object", "invalid"});
			return result;
		}

		[[nodiscard]] bool generated_at_is_closed_utc(std::string_view value) noexcept
		{
			if (value.size() != 20U || value[4U] != '-' || value[7U] != '-' || value[10U] != 'T' ||
				value[13U] != ':' || value[16U] != ':' || value[19U] != 'Z')
				return false;
			for (const auto index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U})
				if (value[index] < '0' || value[index] > '9')
					return false;
			return true;
		}

		[[nodiscard]] const json_value* member(const json_value& value,
											   std::string_view name) noexcept
		{
			return value.member(name);
		}

		[[nodiscard]] bool is_string(const json_value* value,
									 std::string_view expected = {}) noexcept
		{
			if (value == nullptr || value->as_string() == nullptr)
				return false;
			return expected.empty() || *value->as_string() == expected;
		}

		[[nodiscard]] bool is_boolean(const json_value* value, bool expected) noexcept
		{
			return value != nullptr && value->as_boolean() != nullptr &&
				*value->as_boolean() == expected;
		}

		[[nodiscard]] bool is_unsigned(const json_value* value, std::uint64_t expected) noexcept
		{
			return value != nullptr && value->as_unsigned_integer() != nullptr &&
				*value->as_unsigned_integer() == expected;
		}

		[[nodiscard]] bool is_object(const json_value* value) noexcept
		{
			return value != nullptr && value->as_object() != nullptr;
		}

		[[nodiscard]] bool is_array(const json_value* value) noexcept
		{
			return value != nullptr && value->as_array() != nullptr;
		}

		[[nodiscard]] sdk::result<void>
		verify_publication_projection(const json_value& publication,
									  const materialization_store_observation& store)
		{
			if (!is_object(&publication) ||
				!is_string(member(publication, "outcome"), "committed_verified") ||
				!is_string(member(publication, "candidate_identity_state"), "constructed") ||
				!is_string(member(publication, "invocation_commit_state"), "committed") ||
				!is_unsigned(member(publication, "committed_transaction_count"), 1U) ||
				!is_string(member(publication, "candidate_visibility"), "present_by_invocation") ||
				!is_boolean(member(publication, "prior_history_retained"), true) ||
				!is_string(member(publication, "head_effect"), "advanced_to_candidate") ||
				member(publication, "store_failure") == nullptr ||
				!member(publication, "store_failure")->is_null() ||
				!is_object(member(publication, "candidate_identity")) ||
				!is_object(member(publication, "invocation_committed_record")) ||
				!is_object(member(publication, "terminal_head")))
				return sdk::unexpected(
					{"materialization.report-invalid", "publication", "invariant"});

			const auto& record = store.publish_returned_record;
			const auto& candidate = store.candidate_identity;
			if (!store.publication_attempted || store.publish_call_count != 1U || !record ||
				!candidate || !store.publish_returned_handle ||
				record->state != sdk::publication_state::committed || record->corrupt ||
				candidate->publication_id != record->publication_id ||
				candidate->series_id != record->series_id ||
				candidate->snapshot_id != record->snapshot_id ||
				candidate->sequence != record->sequence ||
				candidate->parent_publication != record->parent_publication ||
				store.publish_returned_handle->publication() != *record)
				return sdk::unexpected(
					{"materialization.report-invalid", "publication", "store-unverified"});

			const auto* terminal = member(*member(publication, "terminal_head"), "status");
			if (!is_string(terminal, "present"))
				return sdk::unexpected(
					{"materialization.report-invalid", "publication.terminal_head", "not-present"});
			for (const auto& receipt : store.verification_receipts)
				if (receipt.status != materialization_store_receipt_status::present ||
					!receipt.projection || !receipt.handle)
					return sdk::unexpected(
						{"materialization.report-invalid", "store.verification", "incomplete"});
			return {};
		}

		[[nodiscard]] sdk::result<json_value>
		source_json(const materialization_occurrence_manifest& manifest)
		{
			if (manifest.source_revision.empty() || manifest.source_tree.empty())
				return sdk::unexpected({"materialization.report-invalid", "source", "missing"});
			auto revision = string(manifest.source_revision);
			auto tree = string(manifest.source_tree);
			if (!revision || !tree)
				return sdk::unexpected({"materialization.report-invalid", "source", "string"});
			return make_object({{"revision", std::move(*revision)}, {"tree", std::move(*tree)}});
		}

		[[nodiscard]] sdk::result<json_value>
		request_json(const validated_materialization_request_v2_1& request)
		{
			const auto& identity = request.identity();
			if (identity.materialization_request_id.empty() || identity.request_digest.empty() ||
				identity.semantic_request_digest.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "request", "missing-identity"});
			auto id = string(identity.materialization_request_id);
			auto digest = string(identity.request_digest);
			auto semantic = string(identity.semantic_request_digest);
			if (!id || !digest || !semantic)
				return sdk::unexpected({"materialization.report-invalid", "request", "string"});
			return make_object({{"materialization_request_id", std::move(*id)},
								{"request_digest", std::move(*digest)},
								{"semantic_request_digest", std::move(*semantic)}});
		}

		[[nodiscard]] sdk::result<json_value>
		installation_json(const materialization_v2_1_tool_authority& tool,
						  const materialization_occurrence_manifest& manifest,
						  const materialization_occurrence_receipt& receipt)
		{
			if (tool.occurrence_manifest_digest.empty() || receipt.manifest_file_digest.empty() ||
				receipt.occurrence_payload_digest.empty() || receipt.inventory_digest.empty() ||
				manifest.package_configuration.empty() || manifest.files.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "installation", "missing-authority"});
			object requested;
			requested.emplace("occurrence_manifest_digest",
							  string(tool.occurrence_manifest_digest).value());
			object measured;
			measured.emplace("manifest_path",
							 string(std::string{materialization_occurrence_manifest_path}).value());
			measured.emplace("manifest_file_digest", string(receipt.manifest_file_digest).value());
			measured.emplace("occurrence_payload_digest",
							 string(receipt.occurrence_payload_digest).value());
			measured.emplace("inventory_digest", string(receipt.inventory_digest).value());
			measured.emplace("source_revision", string(manifest.source_revision).value());
			measured.emplace("source_tree", string(manifest.source_tree).value());
			measured.emplace("configuration", string(manifest.package_configuration).value());
			json_value::array_type files;
			files.reserve(manifest.files.size());
			for (const auto& file : manifest.files)
			{
				if (file.role.empty() || file.path.empty() || file.digest.empty())
					return sdk::unexpected({"materialization.report-invalid",
											"installation.measured.files",
											"missing"});
				files.push_back(make_object({{"role", string(file.role).value()},
											 {"path", string(file.path).value()},
											 {"digest", string(file.digest).value()}})
									.value());
			}
			measured.emplace("files", json_value::array(std::move(files)));
			const auto role_path = [&](std::string_view role,
									   std::string_view required_path) -> sdk::result<json_value>
			{
				for (const auto& file : manifest.files)
					if (file.role == role)
					{
						if (file.path != required_path)
							return sdk::unexpected({"materialization.report-invalid",
													"installation." + std::string{role},
													"path"});
						return make_object({{"path", string(file.path).value()},
											{"digest", string(file.digest).value()}});
					}
				return sdk::unexpected({"materialization.report-invalid",
										"installation." + std::string{role},
										"missing"});
			};
			auto materializer =
				role_path("materializer-executable", "bin/cxxlens-clang22-materialize");
			auto worker = role_path("worker-executable", "bin/cxxlens-clang-worker-22");
			if (!materializer || !worker)
				return sdk::unexpected(materializer ? std::move(worker.error())
													: std::move(materializer.error()));
			measured.emplace("tool", std::move(*materializer));
			measured.emplace("worker", std::move(*worker));
			return make_object({{"requested", make_object(std::move(requested)).value()},
								{"measured", make_object(std::move(measured)).value()}});
		}

		[[nodiscard]] sdk::result<json_value>
		provider_json(const materialization_v2_1_tool_authority& tool,
					  const materialization_v2_1_worker_authority& worker)
		{
			if (tool.executable != "cxxlens-clang22-materialize" ||
				tool.interface_version != "2.1.0" ||
				worker.executable != "cxxlens-clang-worker-22" || worker.provider_id.empty() ||
				worker.provider_version.empty() || worker.semantic_contract_digest.empty() ||
				worker.sandbox_policy_digest.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "provider", "authority-mismatch"});
			json_value::array_type features;
			for (const auto& feature : worker.required_features)
				features.push_back(string(feature).value());
			return make_object(
				{{"tool_executable", string(tool.executable).value()},
				 {"tool_interface_version", string(tool.interface_version).value()},
				 {"worker_executable", string(worker.executable).value()},
				 {"provider_id", string(worker.provider_id).value()},
				 {"provider_version", string(worker.provider_version).value()},
				 {"semantic_contract_digest", string(worker.semantic_contract_digest).value()},
				 {"protocol_major", json_value::unsigned_integer(worker.protocol_major)},
				 {"protocol_minor", json_value::unsigned_integer(worker.protocol_minor)},
				 {"required_features", json_value::array(std::move(features))},
				 {"sandbox_policy_digest", string(worker.sandbox_policy_digest).value()}});
		}

		[[nodiscard]] sdk::result<json_value>
		project_json(const prevalidated_materialization_request_v2_1& request)
		{
			const auto& catalog = request.catalog();
			if (request.project_id().empty() || catalog.catalog_id.empty() ||
				catalog.catalog_digest.empty() || catalog.logical_root.empty() ||
				catalog.environment_digest.empty() || catalog.compile_units.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "project", "missing-authority"});
			json_value::array_type units;
			for (const auto& unit : catalog.compile_units)
			{
				if (unit.compile_unit_id.empty() || unit.effective_invocation_digest.empty() ||
					unit.source_digest.empty() || unit.environment_digest.empty())
					return sdk::unexpected({"materialization.report-invalid",
											"project.catalog_compile_units",
											"missing"});
				units.push_back(
					make_object({{"catalog_compile_unit_id", string(unit.compile_unit_id).value()},
								 {"effective_invocation_digest",
								  string(unit.effective_invocation_digest).value()},
								 {"source_digest", string(unit.source_digest).value()},
								 {"environment_digest", string(unit.environment_digest).value()}})
						.value());
			}
			std::vector<sdk::canonical_value> census_values;
			census_values.reserve(catalog.compile_units.size());
			for (const auto& unit : catalog.compile_units)
				census_values.push_back(sdk::canonical_value::from_string(unit.compile_unit_id));
			auto census_bytes =
				sdk::canonical_binary(sdk::canonical_value::from_tuple(std::move(census_values)));
			if (!census_bytes)
				return sdk::unexpected(std::move(census_bytes.error()));
			auto census_digest = sdk::semantic_digest(
				"cxxlens.clang22-catalog-compile-unit-census.v1",
				std::string_view{reinterpret_cast<const char*>(census_bytes->data()),
								 census_bytes->size()});
			if (!census_digest)
				return sdk::unexpected(std::move(census_digest.error()));
			return make_object(
				{{"project_id", string(request.project_id()).value()},
				 {"catalog_id", string(catalog.catalog_id).value()},
				 {"catalog_digest", string(catalog.catalog_digest).value()},
				 {"logical_root", string(catalog.logical_root).value()},
				 {"catalog_environment_digest", string(catalog.environment_digest).value()},
				 {"catalog_compile_unit_census_digest", string(std::move(*census_digest)).value()},
				 {"catalog_compile_units", json_value::array(std::move(units))}});
		}

		[[nodiscard]] sdk::result<json_value> raw_input_json(const raw_input_observation& input)
		{
			if (input.byte_limit == 0U || input.observed_size_bytes > input.byte_limit ||
				input.observed_prefix_digest.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "raw_input_observation", "invalid"});
			return make_object({
				{"byte_limit", json_value::unsigned_integer(input.byte_limit)},
				{"observed_size_bytes", json_value::unsigned_integer(input.observed_size_bytes)},
				{"observed_prefix_digest", string(input.observed_prefix_digest).value()},
				{"complete", json_value::boolean(input.complete)},
			});
		}

		constexpr std::array<std::pair<std::string_view, bool>, 15U> required_supplemental{
			{{"registry", true},
			 {"engine", true},
			 {"interpretation_policy", true},
			 {"trust_policy", true},
			 {"adoption", true},
			 {"task_results", false},
			 {"span_validation", true},
			 {"base_claims", true},
			 {"side_channels", true},
			 {"claim_stages", false},
			 {"provenance", true},
			 {"store", true},
			 {"publication", true},
			 {"semantic_verification", true},
			 {"authority_digests", false}}};
	} // namespace

	sdk::error public_materialization_report_error::as_sdk_error() const
	{
		return report_error(*this);
	}

	public_materialization_success_report_model::public_materialization_success_report_model(
		std::string generated_at,
		std::map<std::string, json_value, utf8_byte_less> fields,
		std::size_t maximum_report_bytes) noexcept
		: generated_at_(std::move(generated_at)), fields_(std::move(fields)),
		  maximum_report_bytes_(maximum_report_bytes)
	{
	}

	std::string_view public_materialization_success_report_model::generated_at() const noexcept
	{
		return generated_at_;
	}

	const std::map<std::string, json_value, utf8_byte_less>&
	public_materialization_success_report_model::fields() const noexcept
	{
		return fields_;
	}

	sdk::result<public_materialization_success_report_model>
	build_public_materialization_success_report(
		const public_materialization_success_report_input& input)
	{
		std::vector<std::string> missing;
		if (input.request == nullptr)
			missing.emplace_back("request");
		if (input.raw_input == nullptr)
			missing.emplace_back("raw_input_observation");
		if (input.occurrence_manifest == nullptr)
			missing.emplace_back("installation.manifest");
		if (input.occurrence_receipt == nullptr)
			missing.emplace_back("installation.receipt");
		if (input.claims == nullptr)
			missing.emplace_back("claims");
		if (input.store == nullptr)
			missing.emplace_back("store.observation");
		if (input.generated_at.empty())
			missing.emplace_back("generated_at");
		for (const auto& [name, _] : required_supplemental)
			if (!input.projections.values.contains(std::string{name}))
				missing.emplace_back(std::string{name});
		if (!missing.empty())
			return sdk::unexpected(
				report_error({public_materialization_report_error_kind::missing_authority,
							  std::move(missing),
							  "report",
							  "required-authority"}));
		if (!generated_at_is_closed_utc(input.generated_at))
			return sdk::unexpected(
				report_error({public_materialization_report_error_kind::invalid_projection,
							  {},
							  "generated_at",
							  "closed-utc-required"}));
		if (input.maximum_report_bytes == 0U)
			return sdk::unexpected(
				report_error({public_materialization_report_error_kind::limit_exceeded,
							  {},
							  "report",
							  "zero-limit"}));

		const auto& request = input.request->request();
		const auto& tool = request.tool();
		const auto& worker = request.worker();
		const auto& claim_batch = input.claims->final_claim_batch();
		if (claim_batch.content_digest.empty() || !claim_batch.unresolved.empty() ||
			!claim_batch.conflicts.empty() || !claim_batch.differential_disagreements.empty() ||
			input.claims->partitions().empty())
			return sdk::unexpected(
				report_error({public_materialization_report_error_kind::invalid_projection,
							  {},
							  "claims",
							  "not-complete"}));
		if (input.occurrence_manifest->occurrence_payload_digest !=
				input.occurrence_receipt->occurrence_payload_digest ||
			input.occurrence_manifest->inventory_digest !=
				input.occurrence_receipt->inventory_digest ||
			input.occurrence_receipt->files.empty())
			return sdk::unexpected(
				report_error({public_materialization_report_error_kind::invalid_projection,
							  {},
							  "installation.receipt",
							  "manifest-mismatch"}));
		if (input.store->first_issue)
			return sdk::unexpected(
				report_error({public_materialization_report_error_kind::publication_unverified,
							  {},
							  "store",
							  "retained-issue"}));
		auto publication = input.projections.values.find("publication");
		if (publication == input.projections.values.end())
			return sdk::unexpected(
				report_error({public_materialization_report_error_kind::missing_authority,
							  {"publication"},
							  "publication",
							  "missing"}));
		if (auto verified = verify_publication_projection(publication->second, *input.store);
			!verified)
			return sdk::unexpected(std::move(verified.error()));
		auto raw_input = raw_input_json(*input.raw_input);
		auto source = source_json(*input.occurrence_manifest);
		auto bound_request = request_json(*input.request);
		auto installation =
			installation_json(tool, *input.occurrence_manifest, *input.occurrence_receipt);
		auto provider = provider_json(tool, worker);
		auto project = project_json(request);
		if (!raw_input || !source || !bound_request || !installation || !provider || !project)
		{
			if (!raw_input)
				return sdk::unexpected(std::move(raw_input.error()));
			if (!source)
				return sdk::unexpected(std::move(source.error()));
			if (!bound_request)
				return sdk::unexpected(std::move(bound_request.error()));
			if (!installation)
				return sdk::unexpected(std::move(installation.error()));
			if (!provider)
				return sdk::unexpected(std::move(provider.error()));
			return sdk::unexpected(std::move(project.error()));
		}

		std::map<std::string, json_value, utf8_byte_less> fields;
		fields.emplace("raw_input_observation", std::move(*raw_input));
		fields.emplace("source", std::move(*source));
		fields.emplace("request", std::move(*bound_request));
		fields.emplace("installation", std::move(*installation));
		fields.emplace("provider", std::move(*provider));
		fields.emplace("project", std::move(*project));
		for (const auto& [name, value] : input.projections.values)
			if (!fields.emplace(name, value).second)
				return sdk::unexpected(
					report_error({public_materialization_report_error_kind::invalid_projection,
								  {},
								  name,
								  "duplicate-derived-field"}));
		for (const auto& [name, object_required] : required_supplemental)
		{
			const auto found = fields.find(std::string{name});
			if (found == fields.end() ||
				(object_required ? !is_object(&found->second) : !is_array(&found->second)))
				return sdk::unexpected(
					report_error({public_materialization_report_error_kind::invalid_projection,
								  {},
								  std::string{name},
								  object_required ? "object-required" : "array-required"}));
		}
		return public_materialization_success_report_model{
			input.generated_at, std::move(fields), input.maximum_report_bytes};
	}

	sdk::result<std::string> encode_public_materialization_success_report(
		const public_materialization_success_report_model& model)
	{
		if (!generated_at_is_closed_utc(model.generated_at_) || model.maximum_report_bytes_ == 0U)
			return sdk::unexpected({"materialization.report-invalid", "report", "model-invalid"});
		object root{{"schema", string("cxxlens.clang22-materialization-report.v2").value()},
					{"report_version", string("2.1.0").value()},
					{"response_kind", string("detailed").value()},
					{"result", string("passed").value()},
					{"generated_at", string(model.generated_at_).value()},
					{"process_exit_status", json_value::unsigned_integer(0U)},
					{"error", json_value::null()}};
		for (const auto& [name, value] : model.fields_)
			if (!root.emplace(name, value).second)
				return sdk::unexpected(
					{"materialization.report-invalid", name, "duplicate-root-member"});
		const auto encoded = canonical_json_line(json_value::object(std::move(root)).value());
		if (encoded.size() > model.maximum_report_bytes_)
			return sdk::unexpected(
				{"materialization.report-invalid", "report", "projection-bytes"});
		return encoded;
	}
} // namespace cxxlens::detail::clang22::materialization
