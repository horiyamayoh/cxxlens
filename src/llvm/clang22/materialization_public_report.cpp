#include "materialization_public_report.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <ranges>
#include <set>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

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

		[[nodiscard]] sdk::result<void> bounded_report_text(const std::string_view value,
															const std::string_view field,
															const bool nonempty = true)
		{
			if ((nonempty && value.empty()) || !sdk::validate_utf8_text(value))
				return sdk::unexpected(
					{"materialization.report-invalid", std::string{field}, "invalid-text"});
			return {};
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		canonical_projection_value(const json_value& value)
		{
			if (value.is_null())
				return sdk::canonical_value::null();
			if (const auto* boolean = value.as_boolean())
				return sdk::canonical_value::from_boolean(*boolean);
			if (const auto* signed_integer = value.as_signed_integer())
				return sdk::canonical_value::from_integer(*signed_integer);
			if (const auto* unsigned_integer = value.as_unsigned_integer())
			{
				if (*unsigned_integer >
					static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					return sdk::unexpected(
						{"materialization.report-invalid", "digest", "unsigned-overflow"});
				return sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(*unsigned_integer));
			}
			if (const auto* text_value = value.as_string())
				return sdk::canonical_value::from_string(*text_value);
			if (const auto* array = value.as_array())
			{
				std::vector<sdk::canonical_value> values;
				values.reserve(array->size());
				for (const auto& child : *array)
				{
					auto projected = canonical_projection_value(child);
					if (!projected)
						return sdk::unexpected(std::move(projected.error()));
					values.push_back(std::move(*projected));
				}
				return sdk::canonical_value::from_tuple(std::move(values));
			}
			const auto* members = value.as_object();
			if (members == nullptr)
				return sdk::unexpected(
					{"materialization.report-invalid", "digest", "unsupported-json-kind"});
			std::vector<sdk::canonical_value> entries;
			entries.reserve(members->size());
			for (const auto& [name, child] : *members)
			{
				auto projected = canonical_projection_value(child);
				if (!projected)
					return sdk::unexpected(std::move(projected.error()));
				entries.push_back(sdk::canonical_value::from_tuple(
					{sdk::canonical_value::from_string(name), std::move(*projected)}));
			}
			return sdk::canonical_value::from_tuple(std::move(entries));
		}

		[[nodiscard]] sdk::result<std::string>
		semantic_projection_digest(const std::string_view domain, const json_value& value)
		{
			auto projected = canonical_projection_value(value);
			if (!projected)
				return sdk::unexpected(std::move(projected.error()));
			auto encoded = sdk::canonical_binary(*projected);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return sdk::semantic_digest(
				domain,
				std::string_view{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		}

		[[nodiscard]] std::string content_digest_text(const std::string_view value)
		{
			return sdk::content_digest(std::as_bytes(std::span{value.data(), value.size()}));
		}

		[[nodiscard]] json_value text_value(const std::string_view value)
		{
			return json_value::string(std::string{value}).value();
		}

		[[nodiscard]] json_value string_array(const std::span<const std::string> values)
		{
			json_value::array_type output;
			output.reserve(values.size());
			for (const auto& value : values)
				output.push_back(text_value(value));
			return json_value::array(std::move(output));
		}

		template <std::size_t N>
		[[nodiscard]] json_value string_array(const std::array<std::string_view, N>& values)
		{
			json_value::array_type output;
			output.reserve(N);
			for (const auto value : values)
				output.push_back(text_value(value));
			return json_value::array(std::move(output));
		}

		[[nodiscard]] sdk::result<json_value>
		task_context_json(const materialization_semantic_task_context& context)
		{
			for (const auto& [field, value] : {
					 std::pair{std::string_view{"provider_task_id"},
							   std::string_view{context.provider_task_id}},
					 std::pair{std::string_view{"task_input_digest"},
							   std::string_view{context.task_input_digest}},
					 std::pair{std::string_view{"selected_catalog_compile_unit_id"},
							   std::string_view{context.selected_catalog_compile_unit_id}},
					 std::pair{std::string_view{"compile_unit_id"},
							   std::string_view{context.compile_unit_id}},
					 std::pair{std::string_view{"condition_universe_id"},
							   std::string_view{context.condition_universe_id}},
					 std::pair{std::string_view{"condition_id"},
							   std::string_view{context.condition_id}},
					 std::pair{std::string_view{"interpretation_domain"},
							   std::string_view{context.interpretation_domain}},
				 })
				if (auto valid = bounded_report_text(value, field); !valid)
					return sdk::unexpected(std::move(valid.error()));
			return make_object({
				{"provider_task_id", text_value(context.provider_task_id)},
				{"task_input_digest", text_value(context.task_input_digest)},
				{"selected_catalog_compile_unit_id",
				 text_value(context.selected_catalog_compile_unit_id)},
				{"compile_unit_id", text_value(context.compile_unit_id)},
				{"condition_universe_id", text_value(context.condition_universe_id)},
				{"condition_id", text_value(context.condition_id)},
				{"interpretation_domain", text_value(context.interpretation_domain)},
			});
		}

		[[nodiscard]] json_value
		semantic_task_key_json(const materialization_semantic_task_context& context)
		{
			return json_value::array({text_value(context.provider_task_id),
									  text_value(context.task_input_digest),
									  text_value(context.selected_catalog_compile_unit_id),
									  text_value(context.compile_unit_id)});
		}

		[[nodiscard]] json_value
		physical_task_execution_key_json(const detailed_task_report_capture& capture)
		{
			return json_value::array({text_value(capture.provider_task_id),
									  text_value(capture.task_input_digest),
									  text_value(capture.provider_execution_id)});
		}

		struct claim_binding
		{
			const materialization_origin_association* association{};
			const materialization_claim_envelope* final_envelope{};
			const materialization_claim_envelope* assertion_envelope{};
		};

		[[nodiscard]] sdk::result<claim_binding>
		find_claim_binding(const detailed_provider_batch_projection& batch,
						   const detailed_provider_batch_projection::row_projection& row,
						   const materialization_semantic_task_context& context,
						   const sealed_materialization_claims& claims)
		{
			std::optional<claim_binding> found;
			for (const auto& association : claims.origin_associations())
			{
				if (association.originating_task != context ||
					association.sealed_row_digest != row.row_digest)
					continue;
				const auto envelope =
					std::ranges::find(claims.claim_envelopes(),
									  association.stored_claim_ref,
									  [](const materialization_claim_envelope& value)
									  {
										  return value.claim_ref;
									  });
				if (envelope == claims.claim_envelopes().end() ||
					envelope->role != "stored_final" ||
					envelope->value.descriptor != batch.descriptor_id)
					continue;
				if (envelope->value.row.canonical_form() != row.row_canonical_form)
					return sdk::unexpected({"materialization.report-invalid",
											"task_results.batches.row_bindings",
											"claim-row-mismatch"});
				if (found)
					return sdk::unexpected({"materialization.report-invalid",
											"task_results.batches.row_bindings",
											"ambiguous-claim-association"});
				found = claim_binding{&association, &*envelope, &*envelope};
			}
			if (!found)
				return sdk::unexpected({"materialization.report-invalid",
										"task_results.batches.row_bindings",
										"claim-association-missing"});

			const bool canonical = batch.descriptor_id == "cc.call_direct_target.v1" ||
				batch.descriptor_id == "cc.call_site.v1" || batch.descriptor_id == "cc.entity.v1";
			std::vector<const materialization_canonicalization_edge*> edges;
			for (const auto& edge : claims.canonicalization_edges())
				if (edge.final_claim_ref == found->final_envelope->claim_ref)
					edges.push_back(&edge);
			if (canonical)
			{
				if (edges.size() != 1U)
					return sdk::unexpected({"materialization.report-invalid",
											"task_results.batches.row_bindings",
											"canonical-assertion-edge-missing"});
				const auto assertion =
					std::ranges::find(claims.claim_envelopes(),
									  edges.front()->precursor_claim_ref,
									  [](const materialization_claim_envelope& value)
									  {
										  return value.claim_ref;
									  });
				if (assertion == claims.claim_envelopes().end() ||
					assertion->role != "hidden_precursor" ||
					assertion->value.descriptor != batch.descriptor_id ||
					assertion->value.row.canonical_form() != row.row_canonical_form)
					return sdk::unexpected({"materialization.report-invalid",
											"task_results.batches.row_bindings",
											"canonical-assertion-missing"});
				found->assertion_envelope = &*assertion;
			}
			else if (!edges.empty())
				return sdk::unexpected({"materialization.report-invalid",
										"task_results.batches.row_bindings",
										"observation-canonicalization-edge"});
			return std::move(*found);
		}

		[[nodiscard]] sdk::result<std::string>
		span_bundle_digest(const observation_v2_primary_span& span)
		{
			if (span.begin > span.end ||
				span.begin > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
				span.end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
				span.span_id.empty() || span.snapshot.empty() || span.file.empty() ||
				span.role.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "span_validation", "invalid-span"});
			auto projection = make_object({
				{"span_id", text_value(span.span_id)},
				{"snapshot", text_value(span.snapshot)},
				{"file", text_value(span.file)},
				{"begin", json_value::unsigned_integer(span.begin)},
				{"end", json_value::unsigned_integer(span.end)},
				{"role", text_value(span.role)},
				{"read_only", json_value::boolean(span.read_only)},
			});
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			return semantic_projection_digest("cxxlens.source-span-bundle.v2", *projection);
		}

		[[nodiscard]] sdk::result<json_value>
		coverage_record_json(const detailed_coverage_projection& record)
		{
			if (auto valid = bounded_report_text(record.kind, "coverage.kind"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = bounded_report_text(record.id, "coverage.id"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = bounded_report_text(record.state, "coverage.state"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = bounded_report_text(record.reason, "coverage.reason", false); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return make_object({{"kind", text_value(record.kind)},
								{"id", text_value(record.id)},
								{"state", text_value(record.state)},
								{"reason", text_value(record.reason)}});
		}

		[[nodiscard]] sdk::result<std::string>
		coverage_set_digest(const std::string_view plane,
							const materialization_semantic_task_context& context,
							const json_value& records)
		{
			auto projection = make_object(
				{{"semantic_task_key", semantic_task_key_json(context)}, {"records", records}});
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			return semantic_projection_digest(
				"cxxlens.clang22-task-" + std::string{plane} + "-coverage.v1", *projection);
		}

		[[nodiscard]] sdk::result<json_value>
		observation_census_json(const std::string_view descriptor,
								const std::vector<json_value>& rows)
		{
			json_value::array_type ordered = rows;
			const auto context_key = [](const json_value& row)
			{
				const auto* context = row.member("originating_task");
				if (context == nullptr)
					return std::tuple{std::string_view{},
									  std::string_view{},
									  std::string_view{},
									  std::string_view{},
									  std::string_view{},
									  std::string_view{},
									  std::string_view{}};
				const auto text = [context](const std::string_view name) -> std::string_view
				{
					const auto* value = context->member(name);
					return value == nullptr || value->as_string() == nullptr
						? std::string_view{}
						: std::string_view{*value->as_string()};
				};
				return std::tuple{text("provider_task_id"),
								  text("task_input_digest"),
								  text("selected_catalog_compile_unit_id"),
								  text("compile_unit_id"),
								  text("condition_universe_id"),
								  text("condition_id"),
								  text("interpretation_domain")};
			};
			for (const auto& row : ordered)
			{
				const auto* digest = row.member("observation_row_digest");
				const auto* context = row.member("originating_task");
				if (digest == nullptr || digest->as_string() == nullptr || context == nullptr ||
					!context->as_object())
					return sdk::unexpected({"materialization.report-invalid",
											"side_channels.guarantee",
											"census-row"});
				for (const auto name : {"provider_task_id",
										"task_input_digest",
										"selected_catalog_compile_unit_id",
										"compile_unit_id",
										"condition_universe_id",
										"condition_id",
										"interpretation_domain"})
					if (context->member(name) == nullptr ||
						context->member(name)->as_string() == nullptr)
						return sdk::unexpected({"materialization.report-invalid",
												"side_channels.guarantee",
												"census-context"});
			}
			std::ranges::sort(ordered,
							  [&context_key](const json_value& left, const json_value& right)
							  {
								  const auto* left_digest = left.member("observation_row_digest");
								  const auto* right_digest = right.member("observation_row_digest");
								  const auto* left_text =
									  left_digest == nullptr ? nullptr : left_digest->as_string();
								  const auto* right_text =
									  right_digest == nullptr ? nullptr : right_digest->as_string();
								  if (left_text == nullptr || right_text == nullptr)
									  return false;
								  return std::tuple{*left_text, context_key(left)} <
									  std::tuple{*right_text, context_key(right)};
							  });
			std::uint64_t exact{};
			for (const auto& row : ordered)
				if (const auto* value = row.member("exact_equivalence");
					value != nullptr && value->as_boolean() != nullptr && *value->as_boolean())
					++exact;
			auto set_projection = make_object(
				{{"descriptor_id", text_value(descriptor)}, {"rows", json_value::array(ordered)}});
			if (!set_projection)
				return sdk::unexpected(std::move(set_projection.error()));
			auto digest = semantic_projection_digest(
				"cxxlens.clang22-observation-equivalence-set.v1", *set_projection);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			return make_object(
				{{"rows", json_value::array(std::move(ordered))},
				 {"exact_equivalence_count", json_value::unsigned_integer(exact)},
				 {"non_exact_equivalence_count", json_value::unsigned_integer(rows.size() - exact)},
				 {"row_equivalence_set_digest", text_value(*digest)}});
		}

		constexpr std::array<std::string_view, 6U> task_descriptor_ids{
			"cc.call_direct_target.v1",
			"cc.call_site.v1",
			"cc.entity.v1",
			"frontend.clang22.call_observation.v2",
			"frontend.clang22.entity_observation.v2",
			"frontend.clang22.type_observation.v2"};
		constexpr std::array<std::string_view, 3U> canonical_task_descriptor_ids{
			"cc.call_direct_target.v1", "cc.call_site.v1", "cc.entity.v1"};
		constexpr std::array<std::string_view, 3U> observation_task_descriptor_ids{
			"frontend.clang22.call_observation.v2",
			"frontend.clang22.entity_observation.v2",
			"frontend.clang22.type_observation.v2"};
		constexpr std::array<std::string_view, 3U> semantic_coverage_kinds{
			"cc.call-extraction", "cc.entity", "frontend.clang22.observation"};
		constexpr std::array<std::string_view, 6U> base_descriptor_ids{"build.project.v1",
																	   "build.toolchain_context.v1",
																	   "build.variant.v1",
																	   "source.file.v1",
																	   "build.compile_unit.v1",
																	   "source.span.v1"};

		[[nodiscard]] sdk::result<json_value> base_cell_json(const sdk::detached_cell& cell)
		{
			if (cell.state == sdk::cell_state::absent)
				return json_value::null();
			if (cell.state != sdk::cell_state::present || !cell.value)
				return sdk::unexpected(
					{"materialization.report-invalid", "base_claims.row", "unknown-cell"});
			return std::visit(
				[](const auto& value) -> sdk::result<json_value>
				{
					using value_type = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<value_type, bool>)
						return json_value::boolean(value);
					else if constexpr (std::is_same_v<value_type, std::int64_t>)
						return json_value::signed_integer(value);
					else if constexpr (std::is_same_v<value_type, std::uint64_t>)
						return json_value::unsigned_integer(value);
					else if constexpr (std::is_same_v<value_type, std::string>)
					{
						if (!sdk::validate_utf8_text(value))
							return sdk::unexpected({"materialization.report-invalid",
													"base_claims.row",
													"invalid-utf8"});
						return json_value::string(value);
					}
					else
						return sdk::unexpected({"materialization.report-invalid",
												"base_claims.row",
												"bytes-forbidden"});
				},
				*cell.value);
		}

		[[nodiscard]] sdk::result<json_value> base_row_json(const sdk::relation_engine& engine,
															const sdk::detached_row& row)
		{
			auto relation = engine.require_id(row.descriptor_id);
			if (!relation)
				return sdk::unexpected(std::move(relation.error()));
			object fields;
			for (const auto& column : relation->descriptor().columns)
			{
				const auto found = row.cells.find(column.id);
				if (found == row.cells.end())
					return sdk::unexpected(
						{"materialization.report-invalid", "base_claims.row", "missing-column"});
				auto value = base_cell_json(found->second);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				fields.emplace(column.name, std::move(*value));
			}
			return make_object(std::move(fields));
		}

		[[nodiscard]] sdk::result<std::string> base_row_identity(const sdk::relation_engine& engine,
																 const sdk::detached_row& row)
		{
			auto relation = engine.require_id(row.descriptor_id);
			if (!relation || !relation->descriptor().domain_identity.result_column)
				return sdk::unexpected({"materialization.report-invalid",
										"base_claims.row_identity",
										"missing-column"});
			const auto found =
				row.cells.find(*relation->descriptor().domain_identity.result_column);
			if (found == row.cells.end() || found->second.state != sdk::cell_state::present ||
				!found->second.value)
				return sdk::unexpected(
					{"materialization.report-invalid", "base_claims.row_identity", "missing"});
			const auto* value = std::get_if<std::string>(&*found->second.value);
			if (value == nullptr || !sdk::validate_strong_id(*value))
				return sdk::unexpected(
					{"materialization.report-invalid", "base_claims.row_identity", "invalid"});
			return *value;
		}

		[[nodiscard]] sdk::result<std::string> base_row_digest(const sdk::relation_engine& engine,
															   const sdk::detached_row& row)
		{
			auto value = base_row_json(engine, row);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			auto projection = make_object(
				{{"descriptor_id", text_value(row.descriptor_id)}, {"row", std::move(*value)}});
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			return semantic_projection_digest("cxxlens.base-claim-row.v1", *projection);
		}

		[[nodiscard]] sdk::result<std::string> row_string_cell(const sdk::detached_row& row,
															   const std::string_view column)
		{
			const auto found = row.cells.find(std::string{column});
			if (found == row.cells.end() || found->second.state != sdk::cell_state::present ||
				!found->second.value)
				return sdk::unexpected(
					{"materialization.report-invalid", "span_validation.row", "missing-string"});
			const auto* value = std::get_if<std::string>(&*found->second.value);
			if (value == nullptr || !sdk::validate_utf8_text(*value))
				return sdk::unexpected(
					{"materialization.report-invalid", "span_validation.row", "string"});
			return *value;
		}

		[[nodiscard]] sdk::result<std::uint64_t> row_unsigned_cell(const sdk::detached_row& row,
																   const std::string_view column)
		{
			const auto found = row.cells.find(std::string{column});
			if (found == row.cells.end() || found->second.state != sdk::cell_state::present ||
				!found->second.value)
				return sdk::unexpected(
					{"materialization.report-invalid", "span_validation.row", "missing-integer"});
			if (const auto* value = std::get_if<std::uint64_t>(&*found->second.value))
				return *value;
			if (const auto* value = std::get_if<std::int64_t>(&*found->second.value);
				value != nullptr && *value >= 0)
				return static_cast<std::uint64_t>(*value);
			return sdk::unexpected(
				{"materialization.report-invalid", "span_validation.row", "integer"});
		}

		[[nodiscard]] sdk::result<bool> row_boolean_cell(const sdk::detached_row& row,
														 const std::string_view column)
		{
			const auto found = row.cells.find(std::string{column});
			if (found == row.cells.end() || found->second.state != sdk::cell_state::present ||
				!found->second.value)
				return sdk::unexpected(
					{"materialization.report-invalid", "span_validation.row", "missing-boolean"});
			const auto* value = std::get_if<bool>(&*found->second.value);
			if (value == nullptr)
				return sdk::unexpected(
					{"materialization.report-invalid", "span_validation.row", "boolean"});
			return *value;
		}

		[[nodiscard]] sdk::result<observation_v2_primary_span>
		span_from_row(const sdk::detached_row& row)
		{
			if (row.descriptor_id != "source.span.v1")
				return sdk::unexpected(
					{"materialization.report-invalid", "span_validation.row", "descriptor"});
			auto span_id = row_string_cell(row, "source.span.v1.span");
			auto snapshot = row_string_cell(row, "source.span.v1.snapshot");
			auto file = row_string_cell(row, "source.span.v1.file");
			auto begin = row_unsigned_cell(row, "source.span.v1.begin");
			auto end = row_unsigned_cell(row, "source.span.v1.end");
			auto role = row_string_cell(row, "source.span.v1.role");
			auto read_only = row_boolean_cell(row, "source.span.v1.read_only");
			if (!span_id || !snapshot || !file || !begin || !end || !role || !read_only)
				return sdk::unexpected(span_id		  ? span_id.error()
										   : snapshot ? snapshot.error()
										   : file	  ? file.error()
										   : begin	  ? begin.error()
										   : end	  ? end.error()
										   : role	  ? role.error()
													  : read_only.error());
			return observation_v2_primary_span{std::move(*span_id),
											   std::move(*snapshot),
											   std::move(*file),
											   *begin,
											   *end,
											   std::move(*role),
											   *read_only};
		}

		[[nodiscard]] sdk::result<json_value> span_json(const observation_v2_primary_span& span)
		{
			if (span.begin > span.end ||
				span.begin > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
				span.end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
				!sdk::validate_strong_id(span.span_id) || !sdk::validate_strong_id(span.snapshot) ||
				!sdk::validate_strong_id(span.file) || !sdk::validate_strong_id(span.role))
				return sdk::unexpected(
					{"materialization.report-invalid", "span_validation.bundle", "identity"});
			return make_object({{"span_id", text_value(span.span_id)},
								{"snapshot", text_value(span.snapshot)},
								{"file", text_value(span.file)},
								{"begin", json_value::unsigned_integer(span.begin)},
								{"end", json_value::unsigned_integer(span.end)},
								{"role", text_value(span.role)},
								{"read_only", json_value::boolean(span.read_only)}});
		}

		struct base_row_record
		{
			std::string descriptor_id;
			std::string row_identity;
			std::string row_digest;
			std::string row_canonical_form;
			sdk::detached_row row;
		};

		[[nodiscard]] materialization_semantic_task_context
		task_context(const detailed_task_report_capture& capture);

		/**
		 * One read-only task-report view used by the detailed report projector.
		 *
		 * The installed production path supplies the sealed replayable spool, while the accumulator
		 * remains available for bounded unit fixtures.  The callback is invoked while one capture
		 * is live and must not retain a reference after it returns.
		 */
		class task_report_source
		{
		  public:
			using consumer = std::function<sdk::result<void>(const detailed_task_report_capture&)>;

			task_report_source(const detailed_task_report_accumulator& accumulator) noexcept
				: accumulator_{&accumulator}
			{
			}
			task_report_source(const detailed_task_report_replayable_spool& spool) noexcept
				: spool_{&spool}
			{
			}

			[[nodiscard]] std::size_t task_count() const noexcept
			{
				return accumulator_ != nullptr ? accumulator_->tasks().size()
											   : spool_->task_count();
			}

			[[nodiscard]] sdk::result<void> replay(const consumer& consume) const
			{
				if (!consume)
					return sdk::unexpected(
						{"materialization.report-invalid", "task_results", "consumer"});
				if (accumulator_ != nullptr)
				{
					for (const auto& capture : accumulator_->tasks())
						if (auto accepted = consume(capture); !accepted)
							return accepted;
					return {};
				}
				return spool_->replay(
					[&consume](detailed_task_report_capture&& capture) -> sdk::result<void>
					{
						return consume(capture);
					});
			}

		  private:
			const detailed_task_report_accumulator* accumulator_{};
			const detailed_task_report_replayable_spool* spool_{};
		};

		[[nodiscard]] sdk::result<std::vector<base_row_record>>
		collect_base_rows(const task_report_source& source,
						  const sdk::relation_engine& engine,
						  const detailed_report_limits& limits)
		{
			std::map<std::pair<std::string, std::string>, base_row_record, std::less<>> unique;
			const auto add = [&](const sdk::detached_row& row) -> sdk::result<void>
			{
				if (row.descriptor_id.empty() ||
					row.canonical_form().size() > limits.max_string_bytes)
					return sdk::unexpected(
						{"materialization.report-invalid", "base_claims", "row-limit"});
				auto identity = base_row_identity(engine, row);
				auto digest = base_row_digest(engine, row);
				if (!identity || !digest)
					return sdk::unexpected(identity ? std::move(digest.error())
													: std::move(identity.error()));
				const auto key = std::pair{row.descriptor_id, *identity};
				const auto found = unique.find(key);
				if (found != unique.end())
				{
					if (found->second.row_digest != *digest ||
						found->second.row_canonical_form != row.canonical_form())
						return sdk::unexpected(
							{"materialization.report-invalid", "base_claims", "conflicting-row"});
					return {};
				}
				unique.emplace(
					key,
					base_row_record{
						row.descriptor_id, *identity, *digest, row.canonical_form(), row});
				return {};
			};
			auto replayed = source.replay(
				[&](const detailed_task_report_capture& capture) -> sdk::result<void>
				{
					if (capture.base_claim_rows.size() != 5U)
						return sdk::unexpected(
							{"materialization.report-invalid", "base_claims", "base-row-count"});
					for (const auto& row : capture.base_claim_rows)
						if (auto added = add(row); !added)
							return added;
					for (const auto& row : capture.source_span_claim_rows)
						if (auto added = add(row); !added)
							return added;
					return {};
				});
			if (!replayed)
				return sdk::unexpected(std::move(replayed.error()));
			if (unique.empty())
				return sdk::unexpected({"materialization.report-invalid", "base_claims", "empty"});
			std::vector<base_row_record> output;
			output.reserve(unique.size());
			for (auto& [_, row] : unique)
				output.push_back(std::move(row));
			const auto rank = [](const std::string_view descriptor)
			{
				const auto found = std::ranges::find(base_descriptor_ids, descriptor);
				return static_cast<std::size_t>(found - base_descriptor_ids.begin());
			};
			std::ranges::sort(
				output,
				[&](const base_row_record& left, const base_row_record& right)
				{
					return std::tuple{
							   rank(left.descriptor_id), left.row_identity, left.row_digest} <
						std::tuple{rank(right.descriptor_id), right.row_identity, right.row_digest};
				});
			return output;
		}

		struct span_binding_record
		{
			observation_v2_primary_span bundle;
			std::string bundle_digest;
			std::string row_digest;
			std::string observation_descriptor_id;
			std::string observation_row_digest;
			materialization_semantic_task_context context;
		};

		[[nodiscard]] sdk::result<std::vector<span_binding_record>>
		collect_span_bindings(const task_report_source& source,
							  const std::vector<base_row_record>& rows)
		{
			std::vector<span_binding_record> output;
			std::set<std::tuple<std::string, std::string, std::string, std::string, std::string>>
				seen;
			auto replayed = source.replay(
				[&](const detailed_task_report_capture& capture) -> sdk::result<void>
				{
					const auto context = task_context(capture);
					for (const auto& observation : capture.observation_rows)
					{
						if (!observation.primary_span)
							continue;
						if (observation.batch_index >= capture.batches.size())
							return sdk::unexpected({"materialization.report-invalid",
													"span_validation",
													"batch-index"});
						const auto& batch = capture.batches[observation.batch_index];
						if (batch.descriptor_id != "frontend.clang22.call_observation.v2" &&
							batch.descriptor_id != "frontend.clang22.entity_observation.v2")
							return sdk::unexpected({"materialization.report-invalid",
													"span_validation",
													"descriptor"});
						auto bundle_digest = span_bundle_digest(*observation.primary_span);
						if (!bundle_digest)
							return sdk::unexpected(std::move(bundle_digest.error()));
						const auto found = std::ranges::find_if(
							rows,
							[&](const base_row_record& row)
							{
								return row.descriptor_id == "source.span.v1" &&
									row.row_identity == observation.primary_span->span_id;
							});
						if (found == rows.end())
							return sdk::unexpected({"materialization.report-invalid",
													"span_validation",
													"source-span-row-missing"});
						auto row_span = span_from_row(found->row);
						if (!row_span || *row_span != *observation.primary_span)
							return sdk::unexpected({"materialization.report-invalid",
													"span_validation",
													"bundle-row-mismatch"});
						const auto key = std::tuple{observation.primary_span->span_id,
													context.provider_task_id,
													batch.descriptor_id,
													observation.observation_row_digest,
													*bundle_digest};
						if (!seen.insert(key).second)
							return sdk::unexpected({"materialization.report-invalid",
													"span_validation",
													"duplicate-binding"});
						output.push_back({*observation.primary_span,
										  *bundle_digest,
										  found->row_digest,
										  batch.descriptor_id,
										  observation.observation_row_digest,
										  context});
					}
					return {};
				});
			if (!replayed)
				return sdk::unexpected(std::move(replayed.error()));
			std::ranges::sort(output,
							  [](const span_binding_record& left, const span_binding_record& right)
							  {
								  return std::tuple{left.bundle.span_id,
													left.context.provider_task_id,
													left.context.task_input_digest,
													left.context.selected_catalog_compile_unit_id,
													left.context.compile_unit_id,
													left.context.condition_universe_id,
													left.context.condition_id,
													left.context.interpretation_domain,
													left.observation_descriptor_id,
													left.observation_row_digest,
													left.bundle_digest} <
									  std::tuple{right.bundle.span_id,
												 right.context.provider_task_id,
												 right.context.task_input_digest,
												 right.context.selected_catalog_compile_unit_id,
												 right.context.compile_unit_id,
												 right.context.condition_universe_id,
												 right.context.condition_id,
												 right.context.interpretation_domain,
												 right.observation_descriptor_id,
												 right.observation_row_digest,
												 right.bundle_digest};
							  });
			return output;
		}

		struct base_origin_record
		{
			materialization_semantic_task_context context;
			std::string provenance_kind;
			std::string provenance_subject;
			std::vector<std::pair<std::string, std::string>> evidence;
			std::optional<json_value> source_bundle;
		};

		[[nodiscard]] sdk::result<json_value> base_evidence_digest_projection(
			const std::vector<std::pair<std::string, std::string>>& evidence)
		{
			json_value::array_type values;
			for (const auto& [kind, subject] : evidence)
				values.push_back(make_object({{"kind", text_value(kind)},
											  {"subject_digest", text_value(subject)}})
									 .value());
			return json_value::array(std::move(values));
		}

		[[nodiscard]] sdk::result<std::string>
		catalog_entry_evidence_digest(const prevalidated_materialization_request_v2_1& request,
									  const std::string_view compile_unit_id)
		{
			const auto found = std::ranges::find(request.catalog().compile_units,
												 compile_unit_id,
												 &sdk::catalog_compile_unit::compile_unit_id);
			if (found == request.catalog().compile_units.end())
				return sdk::unexpected({"materialization.report-invalid",
										"base_claims.evidence",
										"catalog-entry-missing"});
			auto projection = make_object({
				{"catalog_compile_unit_id", text_value(found->compile_unit_id)},
				{"effective_invocation_digest", text_value(found->effective_invocation_digest)},
				{"source_digest", text_value(found->source_digest)},
				{"environment_digest", text_value(found->environment_digest)},
			});
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			return semantic_projection_digest("cxxlens.clang22-catalog-entry-evidence.v1",
											  *projection);
		}

		[[nodiscard]] bool base_row_belongs_to_capture(const base_row_record& row,
													   const detailed_task_report_capture& capture)
		{
			return std::ranges::any_of(capture.base_claim_rows,
									   [&](const sdk::detached_row& candidate)
									   {
										   return candidate.descriptor_id == row.descriptor_id &&
											   candidate.canonical_form() == row.row_canonical_form;
									   });
		}

		[[nodiscard]] sdk::result<base_origin_record>
		make_base_origin(const base_row_record& row,
						 const detailed_task_report_capture& capture,
						 const prevalidated_materialization_request_v2_1& request,
						 const std::vector<span_binding_record>& span_bindings)
		{
			const auto context = task_context(capture);
			std::vector<std::pair<std::string, std::string>> evidence;
			std::string provenance_kind;
			std::string provenance_subject;
			std::optional<json_value> source_bundle;
			if (row.descriptor_id == "build.project.v1")
			{
				provenance_kind = "request_task_input";
				provenance_subject = capture.task_input_digest;
				evidence.emplace_back("compile_context", capture.catalog_digest);
			}
			else if (row.descriptor_id == "build.toolchain_context.v1")
			{
				provenance_kind = "request_task_input";
				provenance_subject = capture.task_input_digest;
				evidence.emplace_back("compile_context", capture.toolchain_digest);
			}
			else if (row.descriptor_id == "build.variant.v1")
			{
				provenance_kind = "request_task_input";
				provenance_subject = capture.task_input_digest;
				evidence.emplace_back("compile_context", row.row_digest);
			}
			else if (row.descriptor_id == "source.file.v1")
			{
				provenance_kind = "request_task_input";
				provenance_subject = capture.task_input_digest;
				evidence.emplace_back("source_observation", capture.source_content_digest);
			}
			else if (row.descriptor_id == "build.compile_unit.v1")
			{
				provenance_kind = "request_task_input";
				provenance_subject = capture.task_input_digest;
				auto catalog_evidence = catalog_entry_evidence_digest(
					request, capture.selected_catalog_compile_unit_id);
				if (!catalog_evidence)
					return sdk::unexpected(std::move(catalog_evidence.error()));
				evidence.emplace_back("compile_context", std::move(*catalog_evidence));
			}
			else if (row.descriptor_id == "source.span.v1")
			{
				const auto found = std::ranges::find_if(
					span_bindings,
					[&](const span_binding_record& binding)
					{
						return binding.context == context && binding.row_digest == row.row_digest;
					});
				if (found == span_bindings.end())
					return sdk::unexpected({"materialization.report-invalid",
											"base_claims.source.span",
											"origin-missing"});
				provenance_kind = "validated_span_bundle";
				provenance_subject = found->bundle_digest;
				evidence = {{"dynamic_observation", found->observation_row_digest},
							{"source_observation", found->bundle_digest}};
				auto bundle = span_json(found->bundle);
				if (!bundle)
					return sdk::unexpected(std::move(bundle.error()));
				// The source bundle edge is deliberately reduced to the exact fields required by
				// the report schema; the full bundle remains separately digest-bound above.
				auto source_bundle_value = make_object({
					{"bundle_digest", text_value(found->bundle_digest)},
					{"observation_descriptor_id", text_value(found->observation_descriptor_id)},
					{"observation_row_digest", text_value(found->observation_row_digest)},
				});
				if (!source_bundle_value)
					return sdk::unexpected(std::move(source_bundle_value.error()));
				source_bundle = std::move(*source_bundle_value);
			}
			else
				return sdk::unexpected(
					{"materialization.report-invalid", "base_claims", "descriptor"});
			return base_origin_record{context,
									  provenance_kind,
									  provenance_subject,
									  std::move(evidence),
									  std::move(source_bundle)};
		}

		[[nodiscard]] bool base_row_matches_capture(const base_row_record& row,
													const detailed_task_report_capture& capture)
		{
			if (row.descriptor_id == "build.project.v1")
				return true;
			if (row.descriptor_id == "source.span.v1")
				return base_row_belongs_to_capture(row, capture) ||
					std::ranges::any_of(capture.source_span_claim_rows,
										[&](const sdk::detached_row& candidate)
										{
											return candidate.canonical_form() ==
												row.row_canonical_form;
										});
			if (row.descriptor_id == "build.toolchain_context.v1")
			{
				auto value = row_string_cell(row.row, "build.toolchain_context.v1.toolchain");
				return value && *value == capture.toolchain_context_id;
			}
			if (row.descriptor_id == "build.variant.v1")
			{
				auto value = row_string_cell(row.row, "build.variant.v1.variant");
				return value && *value == capture.variant_id;
			}
			if (row.descriptor_id == "source.file.v1")
			{
				auto snapshot = row_string_cell(row.row, "source.file.v1.snapshot");
				auto file = row_string_cell(row.row, "source.file.v1.file");
				return snapshot && file && *snapshot == capture.source_snapshot_id &&
					*file == capture.source_file_id;
			}
			if (row.descriptor_id == "build.compile_unit.v1")
			{
				auto value = row_string_cell(row.row, "build.compile_unit.v1.compile_unit");
				return value && *value == capture.compile_unit_id;
			}
			return false;
		}

		[[nodiscard]] sdk::result<json_value> base_origin_json(const base_origin_record& origin)
		{
			auto context = task_context_json(origin.context);
			if (!context)
				return sdk::unexpected(std::move(context.error()));
			auto evidence = base_evidence_digest_projection(origin.evidence);
			if (!evidence)
				return sdk::unexpected(std::move(evidence.error()));
			return make_object({
				{"originating_task", std::move(*context)},
				{"provenance_edge",
				 make_object({{"kind", text_value(origin.provenance_kind)},
							  {"subject_digest", text_value(origin.provenance_subject)}})
					 .value()},
				{"evidence_edges", std::move(*evidence)},
				{"source_bundle",
				 origin.source_bundle ? *origin.source_bundle : json_value::null()},
			});
		}

		[[nodiscard]] sdk::result<json_value> span_binding_json(const span_binding_record& binding)
		{
			auto bundle = span_json(binding.bundle);
			auto context = task_context_json(binding.context);
			if (!bundle)
				return sdk::unexpected(std::move(bundle.error()));
			if (!context)
				return sdk::unexpected(std::move(context.error()));
			return make_object({
				{"bundle", std::move(*bundle)},
				{"bundle_digest", text_value(binding.bundle_digest)},
				{"row_digest", text_value(binding.row_digest)},
				{"observation_descriptor_id", text_value(binding.observation_descriptor_id)},
				{"observation_row_digest", text_value(binding.observation_row_digest)},
				{"originating_task", std::move(*context)},
			});
		}

		[[nodiscard]] sdk::result<std::string>
		producer_identity_digest(const prevalidated_materialization_request_v2_1& request)
		{
			const auto& tool = request.tool();
			auto projection = make_object({
				{"executable", text_value(tool.executable)},
				{"interface_version", text_value(tool.interface_version)},
				{"distribution_version", text_value(tool.distribution_version)},
				{"source_revision", text_value(tool.source_revision)},
				{"source_tree", text_value(tool.source_tree)},
			});
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			return semantic_projection_digest("cxxlens.base-claim-producer.v1", *projection);
		}

		[[nodiscard]] sdk::result<json_value>
		base_claims_json(const task_report_source& source,
						 const prevalidated_materialization_request_v2_1& request,
						 const std::string_view guarantee_digest,
						 const detailed_report_limits& limits)
		{
			auto rows = collect_base_rows(source, request.engine(), limits);
			if (!rows)
				return sdk::unexpected(std::move(rows.error()));
			auto span_bindings = collect_span_bindings(source, *rows);
			if (!span_bindings)
				return sdk::unexpected(std::move(span_bindings.error()));
			auto producer_digest = producer_identity_digest(request);
			if (!producer_digest)
				return sdk::unexpected(std::move(producer_digest.error()));

			json_value::array_type descriptor_results;
			std::uint64_t total_rows{};
			for (const auto descriptor : base_descriptor_ids)
			{
				std::vector<const base_row_record*> descriptor_rows;
				for (const auto& row : *rows)
					if (row.descriptor_id == descriptor)
						descriptor_rows.push_back(&row);
				if (descriptor_rows.empty())
					return sdk::unexpected(
						{"materialization.report-invalid", "base_claims", "descriptor-empty"});

				json_value::array_type row_values;
				for (const auto* row : descriptor_rows)
				{
					json_value::array_type origins;
					// The source is replayed for each row so this projection never retains all
					// task captures.  The sealed spool remains the only cross-task owner.
					auto replayed = source.replay(
						[&](const detailed_task_report_capture& capture) -> sdk::result<void>
						{
							if (descriptor == "source.span.v1")
							{
								for (const auto& binding : *span_bindings)
									if (binding.row_digest == row->row_digest &&
										binding.context == task_context(capture))
									{
										base_origin_record origin{
											binding.context,
											"validated_span_bundle",
											binding.bundle_digest,
											{{"dynamic_observation",
											  binding.observation_row_digest},
											 {"source_observation", binding.bundle_digest}},
											make_object(
												{
													{"bundle_digest",
													 text_value(binding.bundle_digest)},
													{"observation_descriptor_id",
													 text_value(binding.observation_descriptor_id)},
													{"observation_row_digest",
													 text_value(binding.observation_row_digest)},
												})
												.value()};
										auto origin_json = base_origin_json(origin);
										if (!origin_json)
											return sdk::unexpected(std::move(origin_json.error()));
										origins.push_back(std::move(*origin_json));
									}
							}
							else if (base_row_matches_capture(*row, capture))
							{
								auto origin =
									make_base_origin(*row, capture, request, *span_bindings);
								if (!origin)
									return sdk::unexpected(std::move(origin.error()));
								auto origin_json = base_origin_json(*origin);
								if (!origin_json)
									return sdk::unexpected(std::move(origin_json.error()));
								origins.push_back(std::move(*origin_json));
							}
							return {};
						});
					if (!replayed)
						return sdk::unexpected(std::move(replayed.error()));
					if (origins.empty())
						return sdk::unexpected({"materialization.report-invalid",
												"base_claims.row_envelope_bindings",
												"origin-missing"});
					std::ranges::sort(origins,
									  [](const json_value& left, const json_value& right)
									  {
										  return canonical_json(left) < canonical_json(right);
									  });
					auto binding = make_object({
						{"row_identity", text_value(row->row_identity)},
						{"row_digest", text_value(row->row_digest)},
						{"row_canonical_form", text_value(row->row_canonical_form)},
						{"origin_associations", json_value::array(std::move(origins))},
					});
					if (!binding)
						return sdk::unexpected(std::move(binding.error()));
					row_values.push_back(std::move(*binding));
				}

				auto row_set = json_value::array({});
				json_value::array_type row_json_values;
				for (const auto* row : descriptor_rows)
				{
					auto value = base_row_json(request.engine(), row->row);
					if (!value)
						return sdk::unexpected(std::move(value.error()));
					row_json_values.push_back(std::move(*value));
				}
				row_set = json_value::array(std::move(row_json_values));
				auto row_set_projection = make_object({
					{"descriptor_id", text_value(descriptor)},
					{"rows", row_set},
				});
				if (!row_set_projection)
					return sdk::unexpected(std::move(row_set_projection.error()));
				auto row_set_digest = semantic_projection_digest("cxxlens.base-claim-row-set.v1",
																 *row_set_projection);
				if (!row_set_digest)
					return sdk::unexpected(std::move(row_set_digest.error()));

				auto binding_projection = make_object({
					{"descriptor_id", text_value(descriptor)},
					{"row_envelope_bindings", json_value::array(row_values)},
				});
				if (!binding_projection)
					return sdk::unexpected(std::move(binding_projection.error()));
				auto binding_digest = semantic_projection_digest(
					"cxxlens.base-claim-row-envelope-binding-set.v2", *binding_projection);
				if (!binding_digest)
					return sdk::unexpected(std::move(binding_digest.error()));

				json_value::array_type condition_rows;
				json_value::array_type interpretation_rows;
				json_value::array_type provenance_rows;
				json_value::array_type evidence_rows;
				for (const auto& row_value : row_values)
				{
					const auto* identity = row_value.member("row_identity");
					const auto* digest = row_value.member("row_digest");
					const auto* origins = row_value.member("origin_associations");
					if (identity == nullptr || digest == nullptr || origins == nullptr ||
						!origins->as_array())
						return sdk::unexpected(
							{"materialization.report-invalid", "base_claims", "binding-shape"});
					json_value::array_type contexts;
					json_value::array_type interpretation_contexts;
					json_value::array_type provenance_edges;
					json_value::array_type evidence_edge_groups;
					for (const auto& origin : *origins->as_array())
					{
						const auto* context = origin.member("originating_task");
						const auto* provenance = origin.member("provenance_edge");
						const auto* evidence = origin.member("evidence_edges");
						if (context == nullptr || provenance == nullptr || evidence == nullptr ||
							!evidence->as_array())
							return sdk::unexpected(
								{"materialization.report-invalid", "base_claims", "origin-shape"});
						const auto* provider_task_id = context->member("provider_task_id");
						const auto* task_input_digest = context->member("task_input_digest");
						const auto* selected = context->member("selected_catalog_compile_unit_id");
						const auto* compile = context->member("compile_unit_id");
						const auto* universe = context->member("condition_universe_id");
						const auto* condition = context->member("condition_id");
						const auto* interpretation = context->member("interpretation_domain");
						if (provider_task_id == nullptr || task_input_digest == nullptr ||
							selected == nullptr || compile == nullptr || universe == nullptr ||
							condition == nullptr || interpretation == nullptr)
							return sdk::unexpected(
								{"materialization.report-invalid", "base_claims", "context-shape"});
						contexts.push_back(
							make_object({
											{"provider_task_id", *provider_task_id},
											{"task_input_digest", *task_input_digest},
											{"selected_catalog_compile_unit_id", *selected},
											{"compile_unit_id", *compile},
											{"condition_universe_id", *universe},
											{"condition_id", *condition},
										})
								.value());
						interpretation_contexts.push_back(
							make_object({
											{"provider_task_id", *provider_task_id},
											{"task_input_digest", *task_input_digest},
											{"selected_catalog_compile_unit_id", *selected},
											{"compile_unit_id", *compile},
											{"interpretation_domain", *interpretation},
										})
								.value());
						provenance_edges.push_back(*provenance);
						evidence_edge_groups.push_back(*evidence);
					}
					condition_rows.push_back(
						make_object({{"row_identity", *identity},
									 {"contexts", json_value::array(std::move(contexts))}})
							.value());
					interpretation_rows.push_back(
						make_object(
							{
								{"row_identity", *identity},
								{"contexts", json_value::array(std::move(interpretation_contexts))},
							})
							.value());
					provenance_rows.push_back(
						make_object({
										{"row_identity", *identity},
										{"row_digest", *digest},
										{"provenance_edges",
										 json_value::array(std::move(provenance_edges))},
									})
							.value());
					evidence_rows.push_back(
						make_object({
										{"row_identity", *identity},
										{"row_digest", *digest},
										{"evidence_edges",
										 json_value::array(std::move(evidence_edge_groups))},
									})
							.value());
				}
				auto condition_projection = make_object({
					{"descriptor_id", text_value(descriptor)},
					{"row_bindings", json_value::array(std::move(condition_rows))},
				});
				auto interpretation_projection = make_object({
					{"descriptor_id", text_value(descriptor)},
					{"row_bindings", json_value::array(std::move(interpretation_rows))},
				});
				auto provenance_projection = make_object({
					{"descriptor_id", text_value(descriptor)},
					{"rows", json_value::array(std::move(provenance_rows))},
				});
				auto evidence_projection = make_object({
					{"descriptor_id", text_value(descriptor)},
					{"rows", json_value::array(std::move(evidence_rows))},
				});
				if (!condition_projection || !interpretation_projection || !provenance_projection ||
					!evidence_projection)
					return sdk::unexpected(
						{"materialization.report-invalid", "base_claims", "digest-projection"});
				auto condition_digest = semantic_projection_digest(
					"cxxlens.base-claim-condition-fragment-set.v1", *condition_projection);
				auto interpretation_digest = semantic_projection_digest(
					"cxxlens.base-claim-interpretation-domain-set.v1", *interpretation_projection);
				auto provenance_digest = semantic_projection_digest(
					"cxxlens.base-claim-provenance-edge-set.v2", *provenance_projection);
				auto evidence_digest = semantic_projection_digest(
					"cxxlens.base-claim-evidence-edge-set.v2", *evidence_projection);
				if (!condition_digest || !interpretation_digest || !provenance_digest ||
					!evidence_digest)
					return sdk::unexpected(
						{"materialization.report-invalid", "base_claims", "digest"});
				auto envelope_projection = make_object({
					{"descriptor_id", text_value(descriptor)},
					{"row_set_digest", text_value(*row_set_digest)},
					{"row_envelope_bindings", json_value::array(row_values)},
					{"row_envelope_binding_set_digest", text_value(*binding_digest)},
					{"condition_fragment_set_digest", text_value(*condition_digest)},
					{"interpretation_domain_set_digest", text_value(*interpretation_digest)},
					{"producer_identity_digest", text_value(*producer_digest)},
					{"provenance_edge_set_digest", text_value(*provenance_digest)},
					{"evidence_edge_set_digest", text_value(*evidence_digest)},
					{"guarantee_digest", text_value(guarantee_digest)},
				});
				if (!envelope_projection)
					return sdk::unexpected(std::move(envelope_projection.error()));
				auto envelope_digest = semantic_projection_digest(
					"cxxlens.base-claim-envelope-set.v1", *envelope_projection);
				if (!envelope_digest)
					return sdk::unexpected(std::move(envelope_digest.error()));
				descriptor_results.push_back(
					make_object(
						{
							{"descriptor_id", text_value(descriptor)},
							{"row_count", json_value::unsigned_integer(descriptor_rows.size())},
							{"claim_count", json_value::unsigned_integer(descriptor_rows.size())},
							{"row_set_digest", text_value(*row_set_digest)},
							{"row_envelope_bindings", json_value::array(std::move(row_values))},
							{"row_envelope_binding_set_digest", text_value(*binding_digest)},
							{"condition_fragment_set_digest", text_value(*condition_digest)},
							{"interpretation_domain_set_digest",
							 text_value(*interpretation_digest)},
							{"producer_identity_digest", text_value(*producer_digest)},
							{"provenance_edge_set_digest", text_value(*provenance_digest)},
							{"evidence_edge_set_digest", text_value(*evidence_digest)},
							{"guarantee_digest", text_value(guarantee_digest)},
							{"envelope_set_digest", text_value(*envelope_digest)},
						})
						.value());
				total_rows += static_cast<std::uint64_t>(descriptor_rows.size());
			}
			auto claim_set_digest = semantic_projection_digest(
				"cxxlens.base-claim-set.v1", json_value::array(descriptor_results));
			if (!claim_set_digest)
				return sdk::unexpected(std::move(claim_set_digest.error()));
			return make_object({
				{"descriptor_ids", string_array(base_descriptor_ids)},
				{"stage", text_value("canonical_claim")},
				{"transaction_visibility", text_value("unpublished-until-single-commit")},
				{"descriptor_results", json_value::array(std::move(descriptor_results))},
				{"total_row_count", json_value::unsigned_integer(total_rows)},
				{"total_claim_count", json_value::unsigned_integer(total_rows)},
				{"claim_set_digest", text_value(*claim_set_digest)},
				{"validated_before_hard_references", json_value::boolean(true)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		span_validation_json(const task_report_source& source,
							 const sdk::relation_engine& engine,
							 const detailed_report_limits& limits)
		{
			auto rows = collect_base_rows(source, engine, limits);
			if (!rows)
				return sdk::unexpected(std::move(rows.error()));
			auto bindings = collect_span_bindings(source, *rows);
			if (!bindings)
				return sdk::unexpected(std::move(bindings.error()));
			json_value::array_type binding_values;
			for (const auto& binding : *bindings)
			{
				auto value = span_binding_json(binding);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				binding_values.push_back(std::move(*value));
			}
			auto binding_array = json_value::array(binding_values);
			auto binding_digest = semantic_projection_digest(
				"cxxlens.source-span-bundle-task-binding-set.v2", binding_array);
			if (!binding_digest)
				return sdk::unexpected(std::move(binding_digest.error()));
			std::map<std::pair<std::string, std::string>, json_value, std::less<>> unique_bundles;
			for (const auto& binding : *bindings)
			{
				auto bundle = span_json(binding.bundle);
				if (!bundle)
					return sdk::unexpected(std::move(bundle.error()));
				unique_bundles.emplace(
					std::pair{binding.bundle.span_id, binding.bundle_digest},
					make_object({{"bundle", std::move(*bundle)},
								 {"bundle_digest", text_value(binding.bundle_digest)}})
						.value());
			}
			json_value::array_type bundle_values;
			for (auto& [_, value] : unique_bundles)
				bundle_values.push_back(std::move(value));
			auto bundle_set_digest = semantic_projection_digest("cxxlens.source-span-bundle-set.v2",
																json_value::array(bundle_values));
			if (!bundle_set_digest)
				return sdk::unexpected(std::move(bundle_set_digest.error()));
			json_value::array_type span_rows;
			for (const auto& row : *rows)
				if (row.descriptor_id == "source.span.v1")
				{
					auto value = base_row_json(engine, row.row);
					if (!value)
						return sdk::unexpected(std::move(value.error()));
					span_rows.push_back(std::move(*value));
				}
			auto source_span_set_projection = make_object({
				{"descriptor_id", text_value("source.span.v1")},
				{"rows", json_value::array(span_rows)},
			});
			if (!source_span_set_projection)
				return sdk::unexpected(std::move(source_span_set_projection.error()));
			auto source_span_set_digest = semantic_projection_digest(
				"cxxlens.base-claim-row-set.v1", *source_span_set_projection);
			if (!source_span_set_digest)
				return sdk::unexpected(std::move(source_span_set_digest.error()));
			auto evidence_digest = semantic_projection_digest(
				"cxxlens.source-span-bundle-row-evidence-set.v2", binding_array);
			if (!evidence_digest)
				return sdk::unexpected(std::move(evidence_digest.error()));

			std::uint64_t absent{};
			std::uint64_t entity_absent{};
			std::uint64_t call_absent{};
			auto replayed = source.replay(
				[&](const detailed_task_report_capture& capture) -> sdk::result<void>
				{
					for (const auto& observation : capture.observation_rows)
					{
						if (observation.batch_index >= capture.batches.size())
							return sdk::unexpected({"materialization.report-invalid",
													"span_validation",
													"batch-index"});
						const auto descriptor =
							capture.batches[observation.batch_index].descriptor_id;
						if (descriptor != "frontend.clang22.call_observation.v2" &&
							descriptor != "frontend.clang22.entity_observation.v2")
							continue;
						if (!observation.primary_span)
						{
							++absent;
							if (descriptor == "frontend.clang22.call_observation.v2")
								++call_absent;
							else
								++entity_absent;
						}
					}
					return {};
				});
			if (!replayed)
				return sdk::unexpected(std::move(replayed.error()));
			if (absent != 0U)
				return sdk::unexpected(
					{"materialization.report-invalid", "span_validation", "absent-primary-span"});
			return make_object({
				{"contract", text_value("full-primary-span-bundle-v2")},
				{"bundle_fields",
				 string_array(std::array<std::string_view, 7U>{
					 "span_id", "snapshot", "file", "begin", "end", "role", "read_only"})},
				{"optionality", text_value("entity-and-call-optional-all-or-none")},
				{"origin_evidence", text_value("separately-retained-and-digest-bound")},
				{"observed_bundle_count", json_value::unsigned_integer(bindings->size())},
				{"absent_bundle_count", json_value::unsigned_integer(absent)},
				{"entity_absent_bundle_count", json_value::unsigned_integer(entity_absent)},
				{"call_absent_bundle_count", json_value::unsigned_integer(call_absent)},
				{"absent_bundle_unresolved_count", json_value::unsigned_integer(absent)},
				{"source_dependent_canonical_omission_count",
				 json_value::unsigned_integer(call_absent)},
				{"unique_bundle_count", json_value::unsigned_integer(unique_bundles.size())},
				{"constructed_source_span_claim_count",
				 json_value::unsigned_integer(span_rows.size())},
				{"recomputed_id_mismatch_count", json_value::unsigned_integer(0U)},
				{"invalid_range_count", json_value::unsigned_integer(0U)},
				{"task_binding_mismatch_count", json_value::unsigned_integer(0U)},
				{"hard_references_resolved", json_value::boolean(true)},
				{"validated_bundle_bindings", std::move(binding_array)},
				{"bundle_task_binding_set_digest", text_value(*binding_digest)},
				{"bundle_set_digest", text_value(*bundle_set_digest)},
				{"source_span_claim_set_digest", text_value(*source_span_set_digest)},
				{"evidence_digest", text_value(*evidence_digest)},
			});
		}
		[[nodiscard]] materialization_semantic_task_context
		task_context(const detailed_task_report_capture& capture)
		{
			return {capture.provider_task_id,
					capture.task_input_digest,
					capture.selected_catalog_compile_unit_id,
					capture.compile_unit_id,
					capture.condition_universe_id,
					capture.condition_id,
					capture.interpretation_domain};
		}

		[[nodiscard]] bool is_observation_descriptor(const std::string_view descriptor) noexcept
		{
			return std::ranges::find(observation_task_descriptor_ids, descriptor) !=
				observation_task_descriptor_ids.end();
		}

		[[nodiscard]] bool is_canonical_descriptor(const std::string_view descriptor) noexcept
		{
			return std::ranges::find(canonical_task_descriptor_ids, descriptor) !=
				canonical_task_descriptor_ids.end();
		}

		[[nodiscard]] const detailed_observation_row_projection*
		find_observation_row(const detailed_task_report_capture& capture,
							 const std::size_t batch_index,
							 const std::size_t row_index) noexcept
		{
			for (const auto& observation : capture.observation_rows)
				if (observation.batch_index == batch_index && observation.row_index == row_index)
					return &observation;
			return nullptr;
		}

		[[nodiscard]] sdk::result<std::string> guarantee_profile_digest()
		{
			auto profile = make_object({
				{"profile_id", text_value("cxxlens.clang22-materialization-guarantee-profile.v1")},
				{"materialization_contract_version", text_value("2.1.0")},
				{"assumptions", json_value::array({})},
				{"verification_modalities",
				 json_value::array({text_value("clang22.materialization-sealed.v1"),
									text_value("provider.transcript-sealed.v1"),
									text_value("sdk.claim-envelope-validated.v1")})},
			});
			if (!profile)
				return sdk::unexpected(std::move(profile.error()));
			return semantic_projection_digest(
				"cxxlens.clang22-materialization-guarantee-profile.v1", *profile);
		}

		[[nodiscard]] sdk::result<json_value>
		unresolved_record_json(const detailed_unresolved_projection& record)
		{
			if (auto valid = bounded_report_text(record.code, "unresolved.code"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = bounded_report_text(record.subject, "unresolved.subject"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = bounded_report_text(record.detail, "unresolved.detail", false); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return make_object({{"code", text_value(record.code)},
								{"subject", text_value(record.subject)},
								{"detail", text_value(record.detail)}});
		}

		[[nodiscard]] sdk::result<json_value>
		evidence_record_json(const detailed_evidence_projection& record)
		{
			if (auto valid = bounded_report_text(record.kind, "evidence.kind"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = bounded_report_text(record.subject, "evidence.subject"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = bounded_report_text(record.producer, "evidence.producer"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = bounded_report_text(record.summary, "evidence.summary", false); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return make_object({{"kind", text_value(record.kind)},
								{"subject", text_value(record.subject)},
								{"producer", text_value(record.producer)},
								{"summary", text_value(record.summary)}});
		}

		[[nodiscard]] sdk::result<json_value>
		claim_row_binding_json(const detailed_task_report_capture& capture,
							   const detailed_provider_batch_projection& batch,
							   const detailed_provider_batch_projection::row_projection& row,
							   const std::size_t batch_index,
							   const materialization_semantic_task_context& context,
							   const claim_binding& binding)
		{
			if (row.row_index >= batch.rows.size() || row.row_digest.empty() ||
				row.row_canonical_form.empty())
				return sdk::unexpected({"materialization.report-invalid",
										"task_results.batches.row_bindings",
										"row-identity"});
			const auto* observation = find_observation_row(capture, batch_index, row.row_index);
			const bool observation_batch = is_observation_descriptor(batch.descriptor_id);
			if (observation_batch != (observation != nullptr))
				return sdk::unexpected({"materialization.report-invalid",
										"task_results.batches.row_bindings",
										"observation-binding"});
			if (observation != nullptr && observation->observation_row_digest != row.row_digest)
				return sdk::unexpected({"materialization.report-invalid",
										"task_results.batches.row_bindings",
										"observation-row-digest"});
			if (!is_canonical_descriptor(batch.descriptor_id) && !observation_batch)
				return sdk::unexpected({"materialization.report-invalid",
										"task_results.batches",
										"unknown-descriptor"});

			std::optional<std::string> primary_span_digest;
			std::optional<std::string> limitation_digest;
			if (observation != nullptr)
			{
				if (observation->exact_equivalence && observation->limitation)
					return sdk::unexpected({"materialization.report-invalid",
											"task_results.batches.observation_equivalence_census",
											"exact-with-limitation"});
				if (!observation->exact_equivalence && !observation->limitation)
					return sdk::unexpected({"materialization.report-invalid",
											"task_results.batches.observation_equivalence_census",
											"non-exact-without-limitation"});
				if (observation->primary_span)
				{
					if (batch.descriptor_id == "frontend.clang22.type_observation.v2")
						return sdk::unexpected({"materialization.report-invalid",
												"span_validation",
												"type-observation-span-forbidden"});
					auto digest = span_bundle_digest(*observation->primary_span);
					if (!digest)
						return sdk::unexpected(std::move(digest.error()));
					primary_span_digest = std::move(*digest);
				}
				if (observation->limitation)
					limitation_digest = content_digest_text(*observation->limitation);
			}
			const auto expected_evidence =
				primary_span_digest ? primary_span_digest : limitation_digest;
			if (binding.association == nullptr ||
				binding.association->source_evidence_digest != expected_evidence)
				return sdk::unexpected({"materialization.report-invalid",
										"task_results.batches.row_bindings",
										"source-evidence-binding"});
			if (binding.assertion_envelope == nullptr || binding.final_envelope == nullptr ||
				binding.assertion_envelope->claim_ref.empty() ||
				binding.final_envelope->value.content.empty())
				return sdk::unexpected({"materialization.report-invalid",
										"task_results.batches.row_bindings",
										"claim-envelope"});
			if (binding.assertion_envelope->value.row.canonical_form() != row.row_canonical_form ||
				binding.final_envelope->value.row.canonical_form() != row.row_canonical_form)
				return sdk::unexpected({"materialization.report-invalid",
										"task_results.batches.row_bindings",
										"claim-row-mismatch"});
			auto originating_task = task_context_json(context);
			if (!originating_task)
				return sdk::unexpected(std::move(originating_task.error()));
			return make_object({
				{"row_digest", text_value(row.row_digest)},
				{"row_canonical_form", text_value(row.row_canonical_form)},
				{"worker_assertion_claim_ref", text_value(binding.assertion_envelope->claim_ref)},
				{"final_relation_compile_unit_id", text_value(capture.compile_unit_id)},
				{"originating_task", std::move(*originating_task)},
				{"primary_span_bundle_digest",
				 primary_span_digest ? text_value(*primary_span_digest) : json_value::null()},
				{"exact_equivalence",
				 observation != nullptr ? json_value::boolean(observation->exact_equivalence)
										: json_value::null()},
				{"limitation_digest",
				 limitation_digest ? text_value(*limitation_digest) : json_value::null()},
			});
		}

		[[nodiscard]] sdk::result<void>
		coverage_and_side_channels_json(const detailed_task_report_capture& capture,
										const materialization_semantic_task_context& context,
										json_value& coverage_output,
										std::string& semantic_coverage_digest,
										std::string& transport_coverage_digest)
		{
			json_value::array_type transport_records;
			json_value::array_type semantic_records;
			std::set<std::string, std::less<>> seen_transport;
			std::set<std::string, std::less<>> seen_semantic;
			for (const auto& record : capture.coverage)
			{
				auto projected = coverage_record_json(record);
				if (!projected)
					return sdk::unexpected(std::move(projected.error()));
				if (record.kind == "task")
				{
					if (record.id != capture.provider_task_id || record.state != "covered" ||
						record.reason != "" || !seen_transport.insert(record.id).second)
						return sdk::unexpected({"materialization.report-invalid",
												"task_results.coverage",
												"transport-record"});
					transport_records.push_back(std::move(*projected));
				}
				else if (std::ranges::find(semantic_coverage_kinds, record.kind) !=
						 semantic_coverage_kinds.end())
				{
					if (record.id != capture.provider_task_id || record.state != "covered" ||
						record.reason != "" || !seen_semantic.insert(record.kind).second)
						return sdk::unexpected({"materialization.report-invalid",
												"task_results.coverage",
												"semantic-record"});
					semantic_records.push_back(std::move(*projected));
				}
				else
					return sdk::unexpected({"materialization.report-invalid",
											"task_results.coverage",
											"unknown-kind"});
			}
			if (transport_records.size() != 1U ||
				semantic_records.size() != semantic_coverage_kinds.size())
				return sdk::unexpected(
					{"materialization.report-invalid", "task_results.coverage", "incomplete"});
			const auto transport = json_value::array(transport_records);
			const auto semantic = json_value::array(semantic_records);
			auto transport_digest = coverage_set_digest("transport", context, transport);
			if (!transport_digest)
				return sdk::unexpected(std::move(transport_digest.error()));
			auto semantic_digest = coverage_set_digest("semantic", context, semantic);
			if (!semantic_digest)
				return sdk::unexpected(std::move(semantic_digest.error()));
			transport_coverage_digest = std::move(*transport_digest);
			semantic_coverage_digest = std::move(*semantic_digest);
			auto coverage = make_object({
				{"transport_records", transport},
				{"transport_record_set_digest", text_value(transport_coverage_digest)},
				{"semantic_records", semantic},
				{"semantic_record_set_digest", text_value(semantic_coverage_digest)},
			});
			if (!coverage)
				return sdk::unexpected(std::move(coverage.error()));
			coverage_output = std::move(*coverage);
			return {};
		}

		struct task_result_projection
		{
			materialization_semantic_task_context context;
			json_value physical_task_execution_key{json_value::null()};
			json_value value{json_value::null()};
			std::string task_result_digest;
			std::uint64_t frame_count{};
			std::uint64_t raw_frame_stream_bytes{};
			std::string raw_frame_stream_digest;
			std::string frame_transcript_digest;
		};

		[[nodiscard]] sdk::result<task_result_projection>
		project_task_result(const detailed_task_report_capture& capture,
							const sealed_materialization_claims& claims,
							const detailed_report_limits& limits)
		{
			const auto context = task_context(capture);
			if (capture.batches.size() != task_descriptor_ids.size() ||
				capture.input_protocol_major != 1U || capture.input_protocol_minor != 1U ||
				capture.input_chunk_count != capture.ordered_chunk_digests.size() ||
				capture.raw_frame_stream_bytes == 0U || capture.frame_count == 0U)
				return sdk::unexpected(
					{"materialization.report-invalid", "task_results", "incomplete"});
			if (capture.batches.size() > limits.max_batches_per_task ||
				capture.coverage.size() > limits.max_side_channel_records ||
				capture.unresolved.size() > limits.max_side_channel_records ||
				capture.evidence.size() > limits.max_side_channel_records)
				return sdk::unexpected({"materialization.report-invalid", "task_results", "limit"});
			for (std::size_t index{}; index < capture.batches.size(); ++index)
				if (capture.batches[index].descriptor_id != task_descriptor_ids[index])
					return sdk::unexpected({"materialization.report-invalid",
											"task_results.batches",
											"descriptor-order"});

			auto semantic_task_key = semantic_task_key_json(context);
			auto physical_task_key = physical_task_execution_key_json(capture);
			auto full_context = task_context_json(context);
			if (!full_context)
				return sdk::unexpected(std::move(full_context.error()));
			auto input_digest = text_value(capture.task_input_digest);
			auto input_transfer = make_object({
				{"protocol_version", text_value("1.1.0")},
				{"required_feature", text_value("task-input-chunks-v1")},
				{"task_input_codec", text_value("cxxlens.clang22.task.v3")},
				{"logical_input_bytes", json_value::unsigned_integer(capture.logical_input_bytes)},
				{"logical_input_digest", std::move(input_digest)},
				{"canonical_chunk_bytes",
				 json_value::unsigned_integer(capture.canonical_chunk_bytes)},
				{"chunk_count", json_value::unsigned_integer(capture.input_chunk_count)},
				{"ordered_chunk_payload_digest_set_digest",
				 text_value(capture.ordered_chunk_payload_digest_set_digest)},
			});
			if (!input_transfer)
				return sdk::unexpected(std::move(input_transfer.error()));
			auto runtime_receipt = make_object({
				{"raw_frame_stream_bytes",
				 json_value::unsigned_integer(capture.raw_frame_stream_bytes)},
				{"raw_frame_stream_digest", text_value(capture.raw_frame_stream_digest)},
				{"frame_count", json_value::unsigned_integer(capture.frame_count)},
				{"frame_transcript_digest", text_value(capture.frame_transcript_digest)},
				{"sealed_transcript_digest", text_value(capture.sealed_transcript_digest)},
			});
			if (!runtime_receipt)
				return sdk::unexpected(std::move(runtime_receipt.error()));

			json_value coverage{json_value::null()};
			std::string transport_digest;
			std::string semantic_digest;
			if (auto projected = coverage_and_side_channels_json(
					capture, context, coverage, semantic_digest, transport_digest);
				!projected)
				return sdk::unexpected(std::move(projected.error()));

			json_value::array_type batches;
			batches.reserve(capture.batches.size());
			json_value::array_type group_batch_summaries[2U];
			for (std::size_t batch_index{}; batch_index < capture.batches.size(); ++batch_index)
			{
				const auto& batch = capture.batches[batch_index];
				if (batch.row_count != batch.rows.size() ||
					batch.rows.size() > limits.max_side_channel_records ||
					batch.ordered_chunk_digests.size() > limits.max_chunks_per_batch)
					return sdk::unexpected({"materialization.report-invalid",
											"task_results.batches",
											"row-or-chunk-count"});
				json_value::array_type row_bindings;
				std::set<std::string, std::less<>> assertion_refs;
				std::set<std::string, std::less<>> content_ids;
				std::vector<std::string> provenance_digests;
				for (const auto& row : batch.rows)
				{
					auto binding = find_claim_binding(batch, row, context, claims);
					if (!binding)
						return sdk::unexpected(std::move(binding.error()));
					auto row_value =
						claim_row_binding_json(capture, batch, row, batch_index, context, *binding);
					if (!row_value)
						return sdk::unexpected(std::move(row_value.error()));
					row_bindings.push_back(std::move(*row_value));
					assertion_refs.insert(binding->assertion_envelope->claim_ref);
					content_ids.insert(binding->assertion_envelope->value.content);
					auto provenance_projection = make_object({
						{"descriptor_id", text_value(batch.descriptor_id)},
						{"originating_task", *full_context},
						{"row_digest", text_value(row.row_digest)},
					});
					if (!provenance_projection)
						return sdk::unexpected(std::move(provenance_projection.error()));
					auto provenance = semantic_projection_digest(
						"cxxlens.clang22-fixture-provenance-edge.v2", *provenance_projection);
					if (!provenance)
						return sdk::unexpected(std::move(provenance.error()));
					provenance_digests.push_back(std::move(*provenance));
				}
				std::ranges::sort(provenance_digests);
				json_value::array_type assertion_values;
				for (const auto& value : assertion_refs)
					assertion_values.push_back(text_value(value));
				json_value::array_type content_values;
				for (const auto& value : content_ids)
					content_values.push_back(text_value(value));
				json_value::array_type provenance_values;
				for (const auto& value : provenance_digests)
					provenance_values.push_back(text_value(value));
				auto ordered_chunks = string_array(batch.ordered_chunk_digests);
				auto row_binding_array = json_value::array(row_bindings);
				auto task_key_copy = physical_task_key;
				auto ordered_chunk_set_projection = make_object({
					{"task_execution_key", task_key_copy},
					{"descriptor_id", text_value(batch.descriptor_id)},
					{"ordered_chunk_digests", ordered_chunks},
				});
				auto row_binding_set_projection = make_object({
					{"task_execution_key", task_key_copy},
					{"descriptor_id", text_value(batch.descriptor_id)},
					{"row_bindings", row_binding_array},
				});
				auto claim_content_set_projection = make_object({
					{"task_execution_key", task_key_copy},
					{"descriptor_id", text_value(batch.descriptor_id)},
					{"claim_content_ids", json_value::array(content_values)},
				});
				auto sdk_occurrence_set_projection = make_object({
					{"task_execution_key", task_key_copy},
					{"descriptor_id", text_value(batch.descriptor_id)},
					{"worker_assertion_claim_refs", json_value::array(assertion_values)},
				});
				auto provenance_set_projection = make_object({
					{"task_execution_key", task_key_copy},
					{"descriptor_id", text_value(batch.descriptor_id)},
					{"provenance_edge_digests", json_value::array(provenance_values)},
				});
				if (!ordered_chunk_set_projection || !row_binding_set_projection ||
					!claim_content_set_projection || !sdk_occurrence_set_projection ||
					!provenance_set_projection)
					return sdk::unexpected({"materialization.report-invalid",
											"task_results.batches",
											"digest-projection"});
				auto ordered_chunk_set = semantic_projection_digest(
					"cxxlens.clang22-ordered-chunk-set.v1", *ordered_chunk_set_projection);
				auto row_binding_set = semantic_projection_digest(
					"cxxlens.clang22-batch-row-binding-set.v1", *row_binding_set_projection);
				auto claim_content_set = semantic_projection_digest(
					"cxxlens.clang22-batch-claim-content-set.v2", *claim_content_set_projection);
				auto sdk_occurrence_set = semantic_projection_digest(
					"cxxlens.clang22-batch-sdk-occurrence-set.v1", *sdk_occurrence_set_projection);
				auto provenance_set = semantic_projection_digest(
					"cxxlens.clang22-batch-provenance-edge-set.v1", *provenance_set_projection);
				if (!ordered_chunk_set || !row_binding_set || !claim_content_set ||
					!sdk_occurrence_set || !provenance_set)
					return sdk::unexpected(
						{"materialization.report-invalid", "task_results.batches", "digest"});

				json_value observation_census = json_value::null();
				if (is_observation_descriptor(batch.descriptor_id))
				{
					std::vector<json_value> observation_rows;
					for (const auto& observation : capture.observation_rows)
						if (observation.batch_index == batch_index)
						{
							auto observation_context = task_context_json(context);
							if (!observation_context)
								return sdk::unexpected(std::move(observation_context.error()));
							std::optional<std::string> span_digest;
							if (observation.primary_span)
							{
								auto digest = span_bundle_digest(*observation.primary_span);
								if (!digest)
									return sdk::unexpected(std::move(digest.error()));
								span_digest = std::move(*digest);
							}
							std::optional<std::string> limitation_digest;
							if (observation.limitation)
								limitation_digest = content_digest_text(*observation.limitation);
							auto row_value = make_object({
								{"observation_row_digest",
								 text_value(observation.observation_row_digest)},
								{"final_relation_compile_unit_id",
								 text_value(capture.compile_unit_id)},
								{"originating_task", std::move(*observation_context)},
								{"exact_equivalence",
								 json_value::boolean(observation.exact_equivalence)},
								{"limitation",
								 observation.limitation ? text_value(*observation.limitation)
														: json_value::null()},
								{"limitation_digest",
								 limitation_digest ? text_value(*limitation_digest)
												   : json_value::null()},
							});
							if (!row_value)
								return sdk::unexpected(std::move(row_value.error()));
							observation_rows.push_back(std::move(*row_value));
						}
					auto census = observation_census_json(batch.descriptor_id, observation_rows);
					if (!census)
						return sdk::unexpected(std::move(census.error()));
					observation_census = std::move(*census);
				}

				object batch_projection{
					{"batch_id", text_value(batch.batch_id)},
					{"descriptor_id", text_value(batch.descriptor_id)},
					{"runtime_descriptor_digest", text_value(batch.descriptor_digest)},
					{"dependency_group_id", text_value(batch.dependency_group_id)},
					{"atomic_output_group_id", text_value(batch.atomic_output_group_id)},
					{"row_count", json_value::unsigned_integer(batch.row_count)},
					{"ordered_chunk_set_digest", text_value(*ordered_chunk_set)},
					{"row_binding_set_digest", text_value(*row_binding_set)},
					{"worker_assertion_claim_occurrence_count",
					 json_value::unsigned_integer(assertion_refs.size())},
					{"claim_content_count", json_value::unsigned_integer(content_ids.size())},
					{"claim_content_set_digest", text_value(*claim_content_set)},
					{"sdk_claim_occurrence_set_digest", text_value(*sdk_occurrence_set)},
					{"provenance_edge_set_digest", text_value(*provenance_set)},
					{"sealed", json_value::boolean(true)},
					{"task_execution_key", physical_task_key}};
				if (!observation_census.is_null())
					batch_projection.emplace("observation_equivalence_census", observation_census);
				auto batch_digest = make_object(batch_projection);
				if (!batch_digest)
					return sdk::unexpected(std::move(batch_digest.error()));
				auto batch_digest_value =
					semantic_projection_digest("cxxlens.clang22-batch-result.v1", *batch_digest);
				if (!batch_digest_value)
					return sdk::unexpected(std::move(batch_digest_value.error()));
				// `task_execution_key` authenticates the batch digest but is not part of
				// the schema's emitted batch result object.
				batch_projection.erase("task_execution_key");
				batch_projection.emplace("ordered_chunk_digests", std::move(ordered_chunks));
				batch_projection.emplace("row_bindings", std::move(row_binding_array));
				batch_projection.emplace("worker_assertion_claim_refs",
										 json_value::array(std::move(assertion_values)));
				batch_projection.emplace("claim_content_ids",
										 json_value::array(std::move(content_values)));
				batch_projection.emplace("provenance_edge_digests",
										 json_value::array(std::move(provenance_values)));
				batch_projection.emplace("batch_digest", text_value(*batch_digest_value));
				batches.push_back(json_value::object(std::move(batch_projection)).value());

				const std::size_t group_index =
					batch_index < canonical_task_descriptor_ids.size() ? 0U : 1U;
				group_batch_summaries[group_index].push_back(
					make_object(
						{{"descriptor_id", text_value(batch.descriptor_id)},
						 {"batch_digest", text_value(*batch_digest_value)},
						 {"ordered_chunk_set_digest", text_value(*ordered_chunk_set)},
						 {"row_count", json_value::unsigned_integer(batch.row_count)},
						 {"row_binding_set_digest", text_value(*row_binding_set)},
						 {"worker_assertion_claim_occurrence_count",
						  json_value::unsigned_integer(assertion_refs.size())},
						 {"claim_content_count", json_value::unsigned_integer(content_ids.size())},
						 {"claim_content_set_digest", text_value(*claim_content_set)},
						 {"sdk_claim_occurrence_set_digest", text_value(*sdk_occurrence_set)},
						 {"provenance_edge_set_digest", text_value(*provenance_set)}})
						.value());
			}

			json_value::array_type groups;
			std::array<std::string_view, 2U> group_names{"canonical", "observation"};
			for (std::size_t index{}; index < group_names.size(); ++index)
			{
				const auto descriptors = index == 0U
					? std::span<const std::string_view>{canonical_task_descriptor_ids}
					: std::span<const std::string_view>{observation_task_descriptor_ids};
				json_value::array_type descriptor_values;
				for (const auto descriptor : descriptors)
					descriptor_values.push_back(text_value(descriptor));
				auto group_digest_projection = make_object({
					{"task_execution_key", physical_task_key},
					{"dependency_group_id", text_value(group_names[index])},
					{"atomic_output_group_id", text_value("clang22-atomic")},
					{"descriptor_ids", json_value::array(descriptor_values)},
					{"sealed", json_value::boolean(true)},
					{"batches", json_value::array(group_batch_summaries[index])},
				});
				if (!group_digest_projection)
					return sdk::unexpected(std::move(group_digest_projection.error()));
				auto group_digest = semantic_projection_digest("cxxlens.clang22-group-batch-set.v1",
															   *group_digest_projection);
				if (!group_digest)
					return sdk::unexpected(std::move(group_digest.error()));
				groups.push_back(
					make_object(
						{
							{"dependency_group_id", text_value(group_names[index])},
							{"atomic_output_group_id", text_value("clang22-atomic")},
							{"descriptor_ids", json_value::array(std::move(descriptor_values))},
							{"sealed", json_value::boolean(true)},
							{"batch_set_digest", text_value(*group_digest)},
						})
						.value());
			}

			json_value::array_type unresolved_records;
			for (const auto& record : capture.unresolved)
			{
				auto value = unresolved_record_json(record);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				unresolved_records.push_back(std::move(*value));
			}
			if (capture.evidence.size() != 1U ||
				capture.evidence.front().kind != "provider.clang22.execution" ||
				capture.evidence.front().subject != capture.provider_task_id ||
				capture.evidence.front().producer != "cxxlens.clang22.reference" ||
				capture.evidence.front().summary != "exact")
				return sdk::unexpected({"materialization.report-invalid",
										"task_results.evidence",
										"provider-evidence"});
			json_value::array_type evidence_records;
			for (const auto kind : {std::string_view{"canonicalization"},
									std::string_view{"provider_execution"},
									std::string_view{"source_observation"}})
			{
				const detailed_evidence_projection derived{std::string{kind},
														   capture.compile_unit_id,
														   capture.provider_task_id,
														   std::string{kind} + "-retained"};
				auto value = evidence_record_json(derived);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				evidence_records.push_back(std::move(*value));
			}
			auto unresolved_projection =
				make_object({{"originating_task", *full_context},
							 {"records", json_value::array(unresolved_records)}});
			auto evidence_projection =
				make_object({{"originating_task", *full_context},
							 {"records", json_value::array(evidence_records)}});
			if (!unresolved_projection || !evidence_projection)
				return sdk::unexpected(
					{"materialization.report-invalid", "task_results", "side-channel"});
			auto unresolved_digest = semantic_projection_digest(
				"cxxlens.clang22-task-unresolved.v1", *unresolved_projection);
			auto evidence_digest = semantic_projection_digest("cxxlens.clang22-task-evidence.v1",
															  *evidence_projection);
			if (!unresolved_digest || !evidence_digest)
				return sdk::unexpected(
					{"materialization.report-invalid", "task_results", "side-channel-digest"});
			auto profile_digest = guarantee_profile_digest();
			if (!profile_digest)
				return sdk::unexpected(std::move(profile_digest.error()));
			json_value::array_type observation_censuses;
			for (const auto descriptor : observation_task_descriptor_ids)
			{
				const auto batch =
					std::ranges::find(capture.batches,
									  descriptor,
									  [](const detailed_provider_batch_projection& value)
									  {
										  return value.descriptor_id;
									  });
				if (batch == capture.batches.end())
					return sdk::unexpected(
						{"materialization.report-invalid", "task_results", "observation-batch"});
				std::vector<json_value> rows;
				for (const auto& observation : capture.observation_rows)
					if (&capture.batches[observation.batch_index] == &*batch)
					{
						std::optional<std::string> limitation_digest;
						if (observation.limitation)
							limitation_digest = content_digest_text(*observation.limitation);
						auto context_value = task_context_json(context);
						if (!context_value)
							return sdk::unexpected(std::move(context_value.error()));
						auto row = make_object({
							{"observation_row_digest",
							 text_value(observation.observation_row_digest)},
							{"final_relation_compile_unit_id", text_value(capture.compile_unit_id)},
							{"originating_task", std::move(*context_value)},
							{"exact_equivalence",
							 json_value::boolean(observation.exact_equivalence)},
							{"limitation",
							 observation.limitation ? text_value(*observation.limitation)
													: json_value::null()},
							{"limitation_digest",
							 limitation_digest ? text_value(*limitation_digest)
											   : json_value::null()},
						});
						if (!row)
							return sdk::unexpected(std::move(row.error()));
						rows.push_back(std::move(*row));
					}
				auto census = observation_census_json(descriptor, rows);
				if (!census)
					return sdk::unexpected(std::move(census.error()));
				observation_censuses.push_back(
					make_object({
									{"descriptor_id", text_value(descriptor)},
									{"census", std::move(*census)},
								})
						.value());
			}
			json_value::array_type guarantee_groups;
			for (const auto& group : groups)
				guarantee_groups.push_back(
					make_object(
						{
							{"dependency_group_id", *group.member("dependency_group_id")},
							{"atomic_output_group_id", *group.member("atomic_output_group_id")},
							{"descriptor_ids", *group.member("descriptor_ids")},
							{"sealed", *group.member("sealed")},
						})
						.value());
			const auto guarantee_fragment_projection = make_object({
				{"semantic_task_key", semantic_task_key},
				{"profile_id", text_value("cxxlens.clang22-materialization-guarantee-profile.v1")},
				{"profile_digest", text_value(*profile_digest)},
				{"semantic_coverage_set_digest", text_value(semantic_digest)},
				{"unresolved_set_digest", text_value(*unresolved_digest)},
				{"evidence_set_digest", text_value(*evidence_digest)},
				{"groups", json_value::array(std::move(guarantee_groups))},
				{"observation_censuses", json_value::array(observation_censuses)},
			});
			if (!guarantee_fragment_projection)
				return sdk::unexpected(std::move(guarantee_fragment_projection.error()));
			auto guarantee_fragment = semantic_projection_digest(
				"cxxlens.clang22-task-guarantee-fragment.v1", *guarantee_fragment_projection);
			if (!guarantee_fragment)
				return sdk::unexpected(std::move(guarantee_fragment.error()));
			auto side_components = make_object({
				{"transport_coverage_set_digest", text_value(transport_digest)},
				{"semantic_coverage_set_digest", text_value(semantic_digest)},
				{"unresolved_set_digest", text_value(*unresolved_digest)},
				{"evidence_set_digest", text_value(*evidence_digest)},
				{"guarantee_profile_id",
				 text_value("cxxlens.clang22-materialization-guarantee-profile.v1")},
				{"guarantee_profile_digest", text_value(*profile_digest)},
				{"guarantee_fragment_digest", text_value(*guarantee_fragment)},
			});
			if (!side_components)
				return sdk::unexpected(std::move(side_components.error()));
			const auto side_channel_projection = make_object({
				{"task_execution_key", semantic_task_key},
				{"components", *side_components},
			});
			if (!side_channel_projection)
				return sdk::unexpected(std::move(side_channel_projection.error()));
			auto side_channel_digest = semantic_projection_digest(
				"cxxlens.clang22-task-side-channels.v1", *side_channel_projection);
			if (!side_channel_digest)
				return sdk::unexpected(std::move(side_channel_digest.error()));

			json_value::array_type group_digest_values;
			for (const auto& group : groups)
				group_digest_values.push_back(
					make_object({
									{"dependency_group_id",
									 text_value(*group.member("dependency_group_id")->as_string())},
									{"batch_set_digest",
									 text_value(*group.member("batch_set_digest")->as_string())},
								})
						.value());
			auto task_digest_projection = make_object({
				{"task_execution_key", physical_task_key},
				{"selected_catalog_compile_unit_id",
				 text_value(capture.selected_catalog_compile_unit_id)},
				{"compile_unit_id", text_value(capture.compile_unit_id)},
				{"terminal", text_value("provider.success")},
				{"input_transfer", *input_transfer},
				{"runtime_receipt", *runtime_receipt},
				{"coverage", coverage},
				{"groups", json_value::array(std::move(group_digest_values))},
				{"side_channel_digest", text_value(*side_channel_digest)},
			});
			if (!task_digest_projection)
				return sdk::unexpected(std::move(task_digest_projection.error()));
			auto task_digest = semantic_projection_digest("cxxlens.clang22-task-result.v1",
														  *task_digest_projection);
			if (!task_digest)
				return sdk::unexpected(std::move(task_digest.error()));

			auto result = make_object({
				{"provider_task_id", text_value(capture.provider_task_id)},
				{"provider_execution_id", text_value(capture.provider_execution_id)},
				{"selected_catalog_compile_unit_id",
				 text_value(capture.selected_catalog_compile_unit_id)},
				{"compile_unit_id", text_value(capture.compile_unit_id)},
				{"task_input_digest", text_value(capture.task_input_digest)},
				{"terminal", text_value("provider.success")},
				{"input_transfer", std::move(*input_transfer)},
				{"runtime_receipt", std::move(*runtime_receipt)},
				{"coverage", coverage},
				{"groups", json_value::array(std::move(groups))},
				{"batches", json_value::array(std::move(batches))},
				{"side_channel_components", *side_components},
				{"side_channel_digest", text_value(*side_channel_digest)},
				{"task_result_digest", text_value(*task_digest)},
			});
			if (!result)
				return sdk::unexpected(std::move(result.error()));
			return task_result_projection{context,
										  std::move(physical_task_key),
										  std::move(*result),
										  std::move(*task_digest),
										  capture.frame_count,
										  capture.raw_frame_stream_bytes,
										  capture.raw_frame_stream_digest,
										  capture.frame_transcript_digest};
		}

		struct task_results_projection
		{
			json_value results;
			std::string result_set_digest;
			std::uint64_t frame_count{};
			std::string frame_set_digest;
		};

		[[nodiscard]] sdk::result<task_results_projection>
		project_task_results(const task_report_source& source,
							 const sealed_materialization_claims& claims,
							 const detailed_report_limits& limits)
		{
			std::vector<task_result_projection> projected;
			projected.reserve(source.task_count());
			auto replayed = source.replay(
				[&](const detailed_task_report_capture& capture) -> sdk::result<void>
				{
					auto value = project_task_result(capture, claims, limits);
					if (!value)
						return sdk::unexpected(std::move(value.error()));
					projected.push_back(std::move(*value));
					return {};
				});
			if (!replayed)
				return sdk::unexpected(std::move(replayed.error()));
			if (projected.empty() || projected.size() > limits.max_tasks)
				return sdk::unexpected({"materialization.report-invalid", "task_results", "count"});
			std::ranges::sort(
				projected,
				[](const task_result_projection& left, const task_result_projection& right)
				{
					return canonical_json(left.physical_task_execution_key) <
						canonical_json(right.physical_task_execution_key);
				});
			json_value::array_type result_values;
			json_value::array_type result_set_rows;
			json_value::array_type frame_set_rows;
			std::set<std::string, std::less<>> keys;
			std::uint64_t frame_count{};
			for (const auto& task : projected)
			{
				const auto key = canonical_json(task.physical_task_execution_key);
				if (!keys.insert(key).second)
					return sdk::unexpected(
						{"materialization.report-invalid", "task_results", "duplicate-key"});
				result_values.push_back(task.value);
				result_set_rows.push_back(
					make_object({
									{"task_execution_key", task.physical_task_execution_key},
									{"task_result_digest", text_value(task.task_result_digest)},
								})
						.value());
				frame_set_rows.push_back(
					make_object(
						{
							{"task_execution_key", task.physical_task_execution_key},
							{"raw_frame_stream_bytes",
							 json_value::unsigned_integer(task.raw_frame_stream_bytes)},
							{"raw_frame_stream_digest", text_value(task.raw_frame_stream_digest)},
							{"frame_count", json_value::unsigned_integer(task.frame_count)},
							{"frame_transcript_digest", text_value(task.frame_transcript_digest)},
						})
						.value());
				frame_count += task.frame_count;
			}
			auto result_set_projection = json_value::array(result_set_rows);
			auto result_set_digest = semantic_projection_digest(
				"cxxlens.clang22-task-result-set.v1", result_set_projection);
			if (!result_set_digest)
				return sdk::unexpected(std::move(result_set_digest.error()));
			auto frame_set_projection = json_value::array(frame_set_rows);
			auto frame_set_digest = semantic_projection_digest("cxxlens.clang22-raw-frame-set.v1",
															   frame_set_projection);
			if (!frame_set_digest)
				return sdk::unexpected(std::move(frame_set_digest.error()));
			return task_results_projection{json_value::array(std::move(result_values)),
										   std::move(*result_set_digest),
										   frame_count,
										   std::move(*frame_set_digest)};
		}

		[[nodiscard]] sdk::result<std::string>
		json_required_string(const json_value& value,
							 const std::string_view member_name,
							 const std::string_view field)
		{
			const auto* found = value.member(member_name);
			if (found == nullptr || found->as_string() == nullptr || found->as_string()->empty())
				return sdk::unexpected(
					{"materialization.report-invalid", std::string{field}, "string"});
			return *found->as_string();
		}

		[[nodiscard]] sdk::result<json_value>
		semantic_task_component_rows(const json_value& results, const std::string_view component)
		{
			const auto* values = results.as_array();
			if (values == nullptr || values->empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "side_channels", "task-results"});
			json_value::array_type output;
			for (const auto& result : *values)
			{
				auto provider_task_id =
					json_required_string(result, "provider_task_id", "side_channels.task-key");
				auto task_input_digest =
					json_required_string(result, "task_input_digest", "side_channels.task-key");
				auto selected = json_required_string(
					result, "selected_catalog_compile_unit_id", "side_channels.task-key");
				auto compile_unit =
					json_required_string(result, "compile_unit_id", "side_channels.task-key");
				const auto* components = result.member("side_channel_components");
				if (!provider_task_id || !task_input_digest || !selected || !compile_unit ||
					components == nullptr || !components->as_object())
					return sdk::unexpected({"materialization.report-invalid",
											"side_channels",
											"task-component-shape"});
				auto digest =
					json_required_string(*components, component, "side_channels.task-component");
				if (!digest)
					return sdk::unexpected(std::move(digest.error()));
				output.push_back(make_object({
												 {"semantic_task_key",
												  json_value::array({text_value(*provider_task_id),
																	 text_value(*task_input_digest),
																	 text_value(*selected),
																	 text_value(*compile_unit)})},
												 {"component_digest", text_value(*digest)},
											 })
									 .value());
			}
			std::ranges::sort(output,
							  [](const json_value& left, const json_value& right)
							  {
								  return canonical_json(left) < canonical_json(right);
							  });
			return json_value::array(std::move(output));
		}

		[[nodiscard]] sdk::result<json_value>
		global_coverage_summary(const json_value& results,
								const std::string_view plane,
								const std::string_view component,
								const std::string_view record_type,
								const std::string_view digest_domain)
		{
			const auto* values = results.as_array();
			if (values == nullptr || values->empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "side_channels", "empty"});
			json_value::array_type records;
			std::map<std::string, std::uint64_t, std::less<>> state_counts{{"covered", 0U},
																		   {"excluded", 0U},
																		   {"not_applicable", 0U},
																		   {"failed", 0U},
																		   {"unresolved", 0U},
																		   {"unsupported", 0U},
																		   {"stale", 0U},
																		   {"truncated", 0U}};
			for (const auto& result : *values)
			{
				const auto* coverage = result.member("coverage");
				if (coverage == nullptr || !coverage->as_object())
					return sdk::unexpected(
						{"materialization.report-invalid", "side_channels", "coverage-shape"});
				const auto* plane_values = coverage->member(std::string{plane} + "_records");
				if (plane_values == nullptr || !plane_values->as_array())
					return sdk::unexpected(
						{"materialization.report-invalid", "side_channels", "coverage-plane"});
				for (const auto& record : *plane_values->as_array())
				{
					const auto* state = record.member("state");
					if (state == nullptr || state->as_string() == nullptr ||
						!state_counts.contains(*state->as_string()))
						return sdk::unexpected(
							{"materialization.report-invalid", "side_channels", "coverage-state"});
					++state_counts[*state->as_string()];
					records.push_back(record);
				}
			}
			object summary{
				{"record_type", text_value(record_type)},
				{"record_count", json_value::unsigned_integer(records.size())},
				{"state_counts",
				 json_value::object(
					 {
						 {"covered", json_value::unsigned_integer(state_counts["covered"])},
						 {"excluded", json_value::unsigned_integer(state_counts["excluded"])},
						 {"not_applicable",
						  json_value::unsigned_integer(state_counts["not_applicable"])},
						 {"failed", json_value::unsigned_integer(state_counts["failed"])},
						 {"unresolved", json_value::unsigned_integer(state_counts["unresolved"])},
						 {"unsupported", json_value::unsigned_integer(state_counts["unsupported"])},
						 {"stale", json_value::unsigned_integer(state_counts["stale"])},
						 {"truncated", json_value::unsigned_integer(state_counts["truncated"])},
					 })
					 .value()},
				{"balance", text_value("exact")},
			};
			auto components = semantic_task_component_rows(results, component);
			if (!components)
				return sdk::unexpected(std::move(components.error()));
			object digest_projection = summary;
			digest_projection.emplace("task_components", std::move(*components));
			auto digest = semantic_projection_digest(
				digest_domain, json_value::object(std::move(digest_projection)).value());
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			summary.emplace("digest", text_value(*digest));
			return json_value::object(std::move(summary));
		}

		[[nodiscard]] sdk::result<json_value>
		global_observation_census(const json_value& results, const std::string_view descriptor)
		{
			const auto* values = results.as_array();
			if (values == nullptr)
				return sdk::unexpected(
					{"materialization.report-invalid", "side_channels.guarantee", "results"});
			std::vector<json_value> rows;
			for (const auto& result : *values)
			{
				const auto* batches = result.member("batches");
				if (batches == nullptr || !batches->as_array())
					return sdk::unexpected(
						{"materialization.report-invalid", "side_channels.guarantee", "batches"});
				for (const auto& batch : *batches->as_array())
				{
					auto batch_descriptor = json_required_string(
						batch, "descriptor_id", "side_channels.guarantee.descriptor");
					if (!batch_descriptor)
						return sdk::unexpected(std::move(batch_descriptor.error()));
					if (*batch_descriptor != descriptor)
						continue;
					const auto* census = batch.member("observation_equivalence_census");
					if (census == nullptr || !census->as_object())
						return sdk::unexpected({"materialization.report-invalid",
												"side_channels.guarantee",
												"census"});
					const auto* census_rows = census->member("rows");
					if (census_rows == nullptr || !census_rows->as_array())
						return sdk::unexpected({"materialization.report-invalid",
												"side_channels.guarantee",
												"census-rows"});
					rows.insert(rows.end(),
								census_rows->as_array()->begin(),
								census_rows->as_array()->end());
				}
			}
			auto census = observation_census_json(descriptor, rows);
			if (!census)
				return sdk::unexpected(std::move(census.error()));
			return make_object({
				{"descriptor_id", text_value(descriptor)},
				{"rows", *census->member("rows")},
				{"exact_equivalence_count", *census->member("exact_equivalence_count")},
				{"non_exact_equivalence_count", *census->member("non_exact_equivalence_count")},
				{"row_equivalence_set_digest",
				 text_value(*census->member("row_equivalence_set_digest")->as_string())},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		side_channels_json(const json_value& task_results,
						   const prevalidated_materialization_request_v2_1& request)
		{
			auto transport =
				global_coverage_summary(task_results,
										"transport",
										"transport_coverage_set_digest",
										"typed-transport-coverage-unit",
										"cxxlens.clang22-global-transport-coverage.v1");
			auto coverage = global_coverage_summary(task_results,
													"semantic",
													"semantic_coverage_set_digest",
													"typed-coverage-unit",
													"cxxlens.clang22-global-coverage.v1");
			if (!transport || !coverage)
				return sdk::unexpected(!transport ? std::move(transport.error())
												  : std::move(coverage.error()));
			const auto* results = task_results.as_array();
			if (results == nullptr)
				return sdk::unexpected(
					{"materialization.report-invalid", "side_channels", "results"});
			for (const auto& result : *results)
			{
				const auto* unresolved = result.member("side_channel_components");
				if (unresolved == nullptr || !unresolved->as_object())
					return sdk::unexpected(
						{"materialization.report-invalid", "side_channels", "components"});
			}
			object unresolved{
				{"record_type", text_value("typed-unresolved-item")},
				{"record_count", json_value::unsigned_integer(0U)},
				{"blocking_count", json_value::unsigned_integer(0U)},
				{"categories", json_value::array({})},
				{"category_counts", json_value::object({}).value()},
			};
			auto unresolved_components =
				semantic_task_component_rows(task_results, "unresolved_set_digest");
			if (!unresolved_components)
				return sdk::unexpected(std::move(unresolved_components.error()));
			object unresolved_digest_projection = unresolved;
			unresolved_digest_projection.emplace("task_components",
												 std::move(*unresolved_components));
			auto unresolved_digest = semantic_projection_digest(
				"cxxlens.clang22-global-unresolved.v1",
				json_value::object(std::move(unresolved_digest_projection)).value());
			if (!unresolved_digest)
				return sdk::unexpected(std::move(unresolved_digest.error()));
			unresolved.emplace("digest", text_value(*unresolved_digest));

			object evidence{
				{"record_type", text_value("typed-evidence-edge")},
				{"record_count", json_value::unsigned_integer(results->size() * 6U)},
				{"kinds",
				 json_value::array({text_value("canonicalization"),
									text_value("provider_execution"),
									text_value("source_observation")})},
				{"kind_counts",
				 json_value::object(
					 {
						 {"canonicalization", json_value::unsigned_integer(results->size() * 2U)},
						 {"provider_execution", json_value::unsigned_integer(results->size() * 2U)},
						 {"source_observation", json_value::unsigned_integer(results->size() * 2U)},
					 })
					 .value()},
				{"subject_binding", text_value("exact-claim-or-task-identity")},
			};
			auto evidence_components =
				semantic_task_component_rows(task_results, "evidence_set_digest");
			if (!evidence_components)
				return sdk::unexpected(std::move(evidence_components.error()));
			object evidence_digest_projection = evidence;
			evidence_digest_projection.emplace("task_components", std::move(*evidence_components));
			auto evidence_digest = semantic_projection_digest(
				"cxxlens.clang22-global-evidence.v1",
				json_value::object(std::move(evidence_digest_projection)).value());
			if (!evidence_digest)
				return sdk::unexpected(std::move(evidence_digest.error()));
			evidence.emplace("digest", text_value(*evidence_digest));

			auto profile_digest = guarantee_profile_digest();
			if (!profile_digest)
				return sdk::unexpected(std::move(profile_digest.error()));
			json_value::array_type censuses;
			for (const auto descriptor : observation_task_descriptor_ids)
			{
				auto census = global_observation_census(task_results, descriptor);
				if (!census)
					return sdk::unexpected(std::move(census.error()));
				auto descriptor_census = make_object({
					{"descriptor_id", *census->member("descriptor_id")},
					{"exact_equivalence_count", *census->member("exact_equivalence_count")},
					{"non_exact_equivalence_count", *census->member("non_exact_equivalence_count")},
					{"row_equivalence_set_digest",
					 text_value(*census->member("row_equivalence_set_digest")->as_string())},
				});
				if (!descriptor_census)
					return sdk::unexpected(std::move(descriptor_census.error()));
				censuses.push_back(std::move(*descriptor_census));
			}
			object guarantee{
				{"record_type", text_value("typed-guarantee")},
				{"profile_id", text_value("cxxlens.clang22-materialization-guarantee-profile.v1")},
				{"profile_digest", text_value(*profile_digest)},
				{"approximation", text_value("exact")},
				{"scope", text_value(request.project_id())},
				{"assumptions", json_value::array({})},
				{"verification_modalities",
				 json_value::array({text_value("clang22.materialization-sealed.v1"),
									text_value("provider.transcript-sealed.v1"),
									text_value("sdk.claim-envelope-validated.v1")})},
				{"observation_descriptor_censuses", json_value::array(std::move(censuses))},
			};
			object guarantee_digest_projection = guarantee;
			object global_digests{
				{"transport_coverage", *transport->member("digest")},
				{"coverage", *coverage->member("digest")},
				{"unresolved", unresolved.at("digest")},
				{"evidence", evidence.at("digest")},
			};
			guarantee_digest_projection.emplace(
				"global_side_channel_digests",
				json_value::object(std::move(global_digests)).value());
			auto fragments =
				semantic_task_component_rows(task_results, "guarantee_fragment_digest");
			if (!fragments)
				return sdk::unexpected(std::move(fragments.error()));
			guarantee_digest_projection.emplace("task_guarantee_fragments", std::move(*fragments));
			auto guarantee_digest = semantic_projection_digest(
				"cxxlens.clang22-materialization-guarantee.v1",
				json_value::object(std::move(guarantee_digest_projection)).value());
			if (!guarantee_digest)
				return sdk::unexpected(std::move(guarantee_digest.error()));
			guarantee.emplace("digest", text_value(*guarantee_digest));
			return make_object({
				{"transport_coverage", std::move(*transport)},
				{"coverage", std::move(*coverage)},
				{"unresolved", json_value::object(std::move(unresolved)).value()},
				{"evidence", json_value::object(std::move(evidence)).value()},
				{"guarantee", json_value::object(std::move(guarantee)).value()},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		claim_condition_json(const sdk::claim_condition& condition)
		{
			if (auto valid = condition.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			json_value::array_type fragments;
			fragments.reserve(condition.fragments.size());
			for (const auto& fragment : condition.fragments)
				fragments.push_back(text_value(fragment));
			return make_object({{"universe", text_value(condition.universe)},
								{"fragments", json_value::array(std::move(fragments))}});
		}

		[[nodiscard]] sdk::result<json_value>
		claim_producer_json(const sdk::claim_producer& producer)
		{
			if (!sdk::validate_strong_id(producer.id) ||
				!sdk::validate_strong_id(producer.semantic_contract))
				return sdk::unexpected({"materialization.report-invalid",
										"store.claim_envelopes.producer",
										"identity"});
			return make_object({{"id", text_value(producer.id)},
								{"semantic_contract", text_value(producer.semantic_contract)}});
		}

		[[nodiscard]] sdk::result<json_value>
		claim_guarantee_json(const sdk::claim_guarantee& guarantee)
		{
			if (auto valid = guarantee.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			json_value::array_type modalities;
			modalities.reserve(guarantee.verification_modalities.size());
			for (const auto& modality : guarantee.verification_modalities)
				modalities.push_back(text_value(modality));
			return make_object(
				{{"approximation", text_value(guarantee.approximation)},
				 {"scope", text_value(guarantee.scope)},
				 {"assumptions", text_value(guarantee.assumptions)},
				 {"verification_modalities", json_value::array(std::move(modalities))}});
		}

		[[nodiscard]] sdk::result<json_value>
		claim_input_basis_json(const sdk::claim_input_basis& basis)
		{
			const auto* direct = std::get_if<sdk::direct_claim_basis>(&basis);
			if (direct == nullptr || !sdk::validate_strong_id(direct->basis_digest))
				return sdk::unexpected({"materialization.report-invalid",
										"store.claim_envelopes.input_basis",
										"direct-required"});
			return make_object({{"kind", text_value("direct")},
								{"basis_digest", text_value(direct->basis_digest)}});
		}

		[[nodiscard]] sdk::result<json_value> claim_stage_json(const sdk::claim_stage stage)
		{
			switch (stage)
			{
				case sdk::claim_stage::assertion:
					return text_value("assertion");
				case sdk::claim_stage::canonical_claim:
					return text_value("canonical_claim");
				case sdk::claim_stage::derived_claim:
					return sdk::unexpected({"materialization.report-invalid",
											"store.claim_envelopes.stage",
											"derived-forbidden"});
			}
			return sdk::unexpected(
				{"materialization.report-invalid", "store.claim_envelopes.stage", "unknown"});
		}

		[[nodiscard]] sdk::result<json_value>
		claim_envelope_json(const materialization_claim_envelope& envelope)
		{
			const auto& claim = envelope.value;
			if ((envelope.role != "hidden_precursor" && envelope.role != "stored_final") ||
				envelope.row_ref.empty() || envelope.claim_ref.empty() ||
				envelope.sdk_singleton_claim_batch_digest.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "store.claim_envelopes", "identity"});
			if (claim.descriptor.empty() || claim.row.canonical_form().empty() ||
				claim.semantic_key.empty() || claim.assertion.empty() || claim.content.empty() ||
				claim.provenance_root.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "store.claim_envelopes", "incomplete"});
			auto presence = claim_condition_json(claim.presence);
			auto producer = claim_producer_json(claim.producer);
			auto basis = claim_input_basis_json(claim.input_basis);
			auto guarantee = claim_guarantee_json(claim.guarantee);
			auto stage = claim_stage_json(claim.stage);
			if (!presence || !producer || !basis || !guarantee || !stage)
				return sdk::unexpected(!presence		? std::move(presence.error())
										   : !producer	? std::move(producer.error())
										   : !basis		? std::move(basis.error())
										   : !guarantee ? std::move(guarantee.error())
														: std::move(stage.error()));
			return make_object({
				{"claim_ref", text_value(envelope.claim_ref)},
				{"role", text_value(envelope.role)},
				{"row_ref", text_value(envelope.row_ref)},
				{"row_canonical_form", text_value(claim.row.canonical_form())},
				{"descriptor_id", text_value(claim.descriptor)},
				{"semantic_key", text_value(claim.semantic_key)},
				{"assertion", text_value(claim.assertion)},
				{"content", text_value(claim.content)},
				{"presence", std::move(*presence)},
				{"interpretation", text_value(claim.interpretation)},
				{"stage", std::move(*stage)},
				{"producer", std::move(*producer)},
				{"input_basis", std::move(*basis)},
				{"provenance_root", text_value(claim.provenance_root)},
				{"guarantee", std::move(*guarantee)},
				{"sdk_singleton_claim_batch_digest",
				 text_value(envelope.sdk_singleton_claim_batch_digest)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		store_coverage_json(const sdk::snapshot_coverage_unit& unit)
		{
			if (auto valid = unit.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (unit.state != "covered" || !unit.reason.empty())
				return sdk::unexpected({"materialization.report-invalid",
										"store.partitions.coverage_units",
										"non-exact"});
			return make_object({{"domain", text_value(unit.domain)},
								{"key", text_value(unit.key)},
								{"state", text_value(unit.state)},
								{"reason", text_value(unit.reason)}});
		}

		[[nodiscard]] sdk::result<json_value>
		store_unresolved_json(const sdk::unresolved_reference& unresolved)
		{
			json_value::array_type columns;
			for (const auto& column : unresolved.source_columns)
				columns.push_back(text_value(column));
			return make_object({{"source_assertion", text_value(unresolved.source_assertion)},
								{"source_relation", text_value(unresolved.source_relation)},
								{"target_relation", text_value(unresolved.target_relation)},
								{"source_columns", json_value::array(std::move(columns))},
								{"reason", text_value(unresolved.reason)}});
		}

		[[nodiscard]] sdk::result<json_value>
		selector_fields_json(const sdk::snapshot_series_selector& selector)
		{
			if (auto valid = selector.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return make_object(
				{{"catalog_id", text_value(selector.catalog_id)},
				 {"channel_id", text_value(selector.channel_id)},
				 {"engine_generation_id", text_value(selector.engine_generation_id)},
				 {"condition_universe_id", text_value(selector.condition_universe_id)},
				 {"relation_registry_digest", text_value(selector.relation_registry_digest)},
				 {"interpretation_policy_digest",
				  text_value(selector.interpretation_policy_digest)},
				 {"trust_policy_digest", text_value(selector.trust_policy_digest)}});
		}

		[[nodiscard]] sdk::result<json_value>
		selector_json(const sdk::snapshot_series_selector& selector)
		{
			auto fields = selector_fields_json(selector);
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			return make_object(
				{{"fields", std::move(*fields)}, {"series_id", text_value(selector.id())}});
		}

		[[nodiscard]] sdk::result<json_value>
		publication_record_json(const sdk::publication_record& record)
		{
			if (record.state != sdk::publication_state::committed || record.corrupt ||
				record.publication_id.empty() || record.series_id.empty() ||
				record.snapshot_id.empty() || record.sequence == 0U ||
				record.physical_generation == 0U)
				return sdk::unexpected(
					{"materialization.report-invalid", "publication.record", "not-committed"});
			return make_object({
				{"publication_id", text_value(record.publication_id)},
				{"series_id", text_value(record.series_id)},
				{"snapshot_id", text_value(record.snapshot_id)},
				{"sequence", json_value::unsigned_integer(record.sequence)},
				{"physical_generation", json_value::unsigned_integer(record.physical_generation)},
				{"parent_publication",
				 record.parent_publication ? text_value(*record.parent_publication)
										   : json_value::null()},
				{"state", text_value("committed")},
				{"corrupt", json_value::boolean(false)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		publication_identity_json(const sdk::publication_record& record)
		{
			auto full = publication_record_json(record);
			if (!full)
				return sdk::unexpected(std::move(full.error()));
			object value;
			for (const auto name : {std::string_view{"publication_id"},
									std::string_view{"series_id"},
									std::string_view{"snapshot_id"},
									std::string_view{"sequence"},
									std::string_view{"parent_publication"}})
				value.emplace(std::string{name}, *full->member(name));
			return json_value::object(std::move(value));
		}

		[[nodiscard]] sdk::result<json_value>
		partition_json(const materialization_claim_partition& partition)
		{
			const auto& draft = partition.draft;
			const auto& manifest = partition.manifest;
			const auto& binding = partition.binding;
			if (manifest.partition_id != binding.partition_id ||
				manifest.relation_descriptor_id != draft.relation_descriptor_id ||
				manifest.input_basis_digest != draft.producer_input_basis_digest ||
				binding.relation_descriptor_id != draft.relation_descriptor_id ||
				binding.scope != draft.scope || binding.condition != draft.condition ||
				binding.interpretation != draft.interpretation ||
				binding.producer_semantics != draft.producer_semantics ||
				binding.producer_input_basis_digest != draft.producer_input_basis_digest ||
				binding.precision_profile != draft.precision_profile ||
				binding.assumption_set_id != draft.assumption_set_id)
				return sdk::unexpected(
					{"materialization.report-invalid", "store.partitions", "binding-mismatch"});
			auto condition = claim_condition_json(draft.condition);
			if (!condition)
				return sdk::unexpected(std::move(condition.error()));
			json_value::array_type refs;
			for (const auto& ref : partition.stored_claim_refs)
				refs.push_back(text_value(ref));
			std::ranges::sort(refs,
							  [](const json_value& left, const json_value& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			json_value::array_type contents;
			for (const auto& content : partition.claim_content_ids)
				contents.push_back(text_value(content));
			std::ranges::sort(contents,
							  [](const json_value& left, const json_value& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			std::vector<const sdk::snapshot_coverage_unit*> coverage_units;
			coverage_units.reserve(draft.coverage.size());
			for (const auto& unit : draft.coverage)
				coverage_units.push_back(&unit);
			std::ranges::sort(coverage_units,
							  [](const auto* left, const auto* right)
							  {
								  return left->canonical_form() < right->canonical_form();
							  });
			json_value::array_type coverage;
			for (const auto* unit : coverage_units)
			{
				auto value = store_coverage_json(*unit);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				coverage.push_back(std::move(*value));
			}
			json_value::array_type unresolved;
			for (const auto& value : draft.unresolved)
			{
				auto projected = store_unresolved_json(value);
				if (!projected)
					return sdk::unexpected(std::move(projected.error()));
				unresolved.push_back(std::move(*projected));
			}
			return make_object({
				{"relation_descriptor_id", text_value(draft.relation_descriptor_id)},
				{"scope", text_value(draft.scope)},
				{"condition", std::move(*condition)},
				{"interpretation", text_value(draft.interpretation)},
				{"producer_semantics", text_value(draft.producer_semantics)},
				{"producer_input_basis_digest", text_value(draft.producer_input_basis_digest)},
				{"precision_profile", text_value(draft.precision_profile)},
				{"assumption_set_id", text_value(draft.assumption_set_id)},
				{"empty_partition", json_value::boolean(partition.empty_partition)},
				{"stored_claim_refs", json_value::array(std::move(refs))},
				{"claim_content_digests", json_value::array(std::move(contents))},
				{"sdk_claim_occurrence_count",
				 json_value::unsigned_integer(partition.sdk_claim_occurrence_count)},
				{"origin_association_count",
				 json_value::unsigned_integer(partition.origin_association_count)},
				{"coverage_units", json_value::array(std::move(coverage))},
				{"unresolved", json_value::array(std::move(unresolved))},
				{"partition_id", text_value(manifest.partition_id)},
				{"claim_set_digest", text_value(manifest.claim_set_digest)},
				{"coverage_digest", text_value(manifest.coverage_digest)},
				{"content_digest", text_value(manifest.content_digest)},
				{"claim_count", json_value::unsigned_integer(manifest.claim_count)},
				{"complete", json_value::boolean(manifest.complete)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		snapshot_manifest_json(const sdk::snapshot_manifest& manifest)
		{
			if (manifest.schema != "cxxlens.snapshot-manifest.v1" || manifest.id.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "store.snapshot_manifest", "identity"});
			json_value::array_type partitions;
			for (const auto& partition : manifest.partitions)
				partitions.push_back(
					make_object(
						{
							{"partition_id", text_value(partition.partition_id)},
							{"relation_descriptor_id",
							 text_value(partition.relation_descriptor_id)},
							{"input_basis_digest", text_value(partition.input_basis_digest)},
							{"claim_set_digest", text_value(partition.claim_set_digest)},
							{"coverage_digest", text_value(partition.coverage_digest)},
							{"content_digest", text_value(partition.content_digest)},
							{"claim_count", json_value::unsigned_integer(partition.claim_count)},
							{"complete", json_value::boolean(partition.complete)},
						})
						.value());
			std::ranges::sort(partitions,
							  [](const json_value& left, const json_value& right)
							  {
								  return *left.member("partition_id")->as_string() <
									  *right.member("partition_id")->as_string();
							  });
			json_value::array_type closures;
			for (const auto& closure : manifest.closure_ids)
				closures.push_back(text_value(closure));
			std::ranges::sort(closures,
							  [](const json_value& left, const json_value& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			return make_object({
				{"schema", text_value(manifest.schema)},
				{"snapshot_semantics_version",
				 text_value(manifest.snapshot_semantics_version.string())},
				{"catalog_semantic_digest", text_value(manifest.catalog_semantic_digest)},
				{"condition_universe_id", text_value(manifest.condition_universe_id)},
				{"relation_registry_digest", text_value(manifest.relation_registry_digest)},
				{"interpretation_policy_digest", text_value(manifest.interpretation_policy_digest)},
				{"partitions", json_value::array(std::move(partitions))},
				{"closure_ids", json_value::array(std::move(closures))},
				{"snapshot_id", text_value(manifest.id)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		store_json(const prevalidated_materialization_request_v2_1& request,
				   const sealed_materialization_claims& claims,
				   const materialization_store_observation& observation)
		{
			if (observation.first_issue || !observation.publication_attempted ||
				observation.publish_call_count != 1U || !observation.publish_returned_record ||
				!observation.publish_returned_handle || !observation.candidate_manifest ||
				!observation.candidate_identity || !observation.verification_store)
				return sdk::unexpected({"materialization.report-invalid", "store", "unverified"});
			const auto& returned = *observation.publish_returned_record;
			const auto& returned_handle = *observation.publish_returned_handle;
			if (returned.state != sdk::publication_state::committed || returned.corrupt ||
				returned_handle.publication() != returned ||
				returned_handle.manifest() != *observation.candidate_manifest ||
				returned_handle.physical_backend() != observation.backend)
				return sdk::unexpected(
					{"materialization.report-invalid", "store", "publication-mismatch"});
			for (const auto& receipt : observation.verification_receipts)
				if (receipt.status != materialization_store_receipt_status::present ||
					receipt.error || !receipt.projection || !receipt.handle ||
					receipt.projection->publication != returned ||
					receipt.projection->manifest != returned_handle.manifest() ||
					receipt.projection->physical_backend != observation.backend)
					return sdk::unexpected(
						{"materialization.report-invalid", "store.verification", "unverified"});
			const auto& request_publication = request.publication();
			if (observation.backend != request_publication.backend ||
				observation.selector != request_publication.selector ||
				observation.series_id != request_publication.series_id ||
				observation.expected_parent_publication !=
					request_publication.expected_parent_publication)
				return sdk::unexpected(
					{"materialization.report-invalid", "store.selector", "request-mismatch"});

			auto selector = selector_json(observation.selector);
			if (!selector)
				return sdk::unexpected(std::move(selector.error()));
			auto direct_input = sdk::claim_input_basis_digest(
				sdk::direct_claim_basis{std::string{claims.direct_basis_digest()}});
			if (!direct_input)
				return sdk::unexpected(std::move(direct_input.error()));
			auto producer_input = std::string{*direct_input};

			std::vector<const materialization_claim_envelope*> envelopes;
			for (const auto& envelope : claims.claim_envelopes())
				envelopes.push_back(&envelope);
			std::ranges::sort(envelopes,
							  [](const auto* left, const auto* right)
							  {
								  return left->claim_ref < right->claim_ref;
							  });
			json_value::array_type envelope_values;
			std::map<std::string, std::pair<std::string, std::string>, std::less<>> rows;
			for (const auto* envelope : envelopes)
			{
				auto value = claim_envelope_json(*envelope);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				envelope_values.push_back(std::move(*value));
				const auto canonical = envelope->value.row.canonical_form();
				auto [found, inserted] = rows.emplace(
					envelope->row_ref, std::pair{envelope->value.descriptor, canonical});
				if (!inserted && found->second != std::pair{envelope->value.descriptor, canonical})
					return sdk::unexpected({"materialization.report-invalid",
											"store.claim_rows",
											"row-ref-collision"});
			}
			json_value::array_type row_values;
			for (const auto& [row_ref, identity] : rows)
				row_values.push_back(
					make_object({{"row_ref", text_value(row_ref)},
								 {"descriptor_id", text_value(identity.first)},
								 {"row_canonical_form", text_value(identity.second)}})
						.value());

			json_value::array_type edge_values;
			std::vector<const materialization_canonicalization_edge*> edges;
			for (const auto& edge : claims.canonicalization_edges())
				edges.push_back(&edge);
			std::ranges::sort(edges,
							  [](const auto* left, const auto* right)
							  {
								  return std::tie(left->precursor_claim_ref,
												  left->final_claim_ref,
												  left->transform_semantics) <
									  std::tie(right->precursor_claim_ref,
											   right->final_claim_ref,
											   right->transform_semantics);
							  });
			for (const auto* edge : edges)
				edge_values.push_back(
					make_object({
									{"precursor_claim_ref", text_value(edge->precursor_claim_ref)},
									{"final_claim_ref", text_value(edge->final_claim_ref)},
									{"transform_semantics", text_value(edge->transform_semantics)},
								})
						.value());

			json_value::array_type association_values;
			std::vector<const materialization_origin_association*> associations;
			for (const auto& association : claims.origin_associations())
				associations.push_back(&association);
			std::ranges::sort(associations,
							  [](const auto* left, const auto* right)
							  {
								  return left->association_id < right->association_id;
							  });
			for (const auto* association : associations)
			{
				auto context = task_context_json(association->originating_task);
				if (!context)
					return sdk::unexpected(std::move(context.error()));
				association_values.push_back(
					make_object(
						{
							{"association_id", text_value(association->association_id)},
							{"stored_claim_ref", text_value(association->stored_claim_ref)},
							{"originating_task", std::move(*context)},
							{"sealed_row_digest", text_value(association->sealed_row_digest)},
							{"source_evidence_digest",
							 association->source_evidence_digest
								 ? text_value(*association->source_evidence_digest)
								 : json_value::null()},
						})
						.value());
			}

			const auto& final_batch = claims.final_claim_batch();
			std::vector<std::string> final_refs;
			std::set<std::string, std::less<>> final_contents;
			for (const auto* envelope : envelopes)
				if (envelope->role == "stored_final")
				{
					final_refs.push_back(envelope->claim_ref);
					final_contents.insert(envelope->value.content);
				}
			std::ranges::sort(final_refs);
			if (final_refs.size() != final_batch.claims.size() ||
				final_batch.content_digest.empty() || final_batch.unresolved.size() != 0U ||
				final_batch.conflicts.size() != 0U ||
				final_batch.differential_disagreements.size() != 0U)
				return sdk::unexpected({"materialization.report-invalid",
										"store.claim_batch_validation",
										"census-mismatch"});
			auto batch_digest =
				sdk::claim_batch_content_digest(std::span<const sdk::claim>{final_batch.claims},
												final_batch.unresolved,
												final_batch.conflicts,
												final_batch.differential_disagreements);
			if (!batch_digest || *batch_digest != final_batch.content_digest)
				return sdk::unexpected({"materialization.report-invalid",
										"store.claim_batch_validation",
										"digest-mismatch"});
			json_value::array_type final_ref_values;
			for (const auto& ref : final_refs)
				final_ref_values.push_back(text_value(ref));
			auto claim_batch = make_object({
				{"contract", text_value("cxxlens.claim-batch.v2")},
				{"final_claim_refs", json_value::array(std::move(final_ref_values))},
				{"sdk_claim_occurrence_count",
				 json_value::unsigned_integer(final_batch.claims.size())},
				{"unique_claim_content_count", json_value::unsigned_integer(final_contents.size())},
				{"unresolved_count", json_value::unsigned_integer(final_batch.unresolved.size())},
				{"conflict_count", json_value::unsigned_integer(final_batch.conflicts.size())},
				{"differential_disagreement_count",
				 json_value::unsigned_integer(final_batch.differential_disagreements.size())},
				{"content_digest", text_value(final_batch.content_digest)},
			});
			if (!claim_batch)
				return sdk::unexpected(std::move(claim_batch.error()));

			std::vector<const materialization_claim_partition*> partitions;
			for (const auto& partition : claims.partitions())
				partitions.push_back(&partition);
			std::ranges::sort(partitions,
							  [](const auto* left, const auto* right)
							  {
								  return left->manifest.partition_id < right->manifest.partition_id;
							  });
			json_value::array_type partition_values;
			for (const auto* partition : partitions)
			{
				auto value = partition_json(*partition);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				partition_values.push_back(std::move(*value));
			}
			if (partition_values.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "store.partitions", "empty"});
			auto manifest = snapshot_manifest_json(returned_handle.manifest());
			if (!manifest)
				return sdk::unexpected(std::move(manifest.error()));
			if (returned_handle.manifest().partitions.size() != partitions.size())
				return sdk::unexpected({"materialization.report-invalid",
										"store.snapshot_manifest",
										"partition-count"});
			for (const auto& partition : partitions)
				if (!std::ranges::any_of(returned_handle.manifest().partitions,
										 [&](const sdk::partition_manifest& value)
										 {
											 return value == partition->manifest;
										 }))
					return sdk::unexpected({"materialization.report-invalid",
											"store.snapshot_manifest",
											"partition-mismatch"});

			auto direct_basis = make_object({
				{"projection_version",
				 text_value("cxxlens.clang22-direct-materialization-basis.v1")},
				{"materializer_semantics_digest",
				 text_value(claims.materializer_semantics_digest())},
				{"basis_digest", text_value(claims.direct_basis_digest())},
				{"producer_input_basis_digest", text_value(producer_input)},
				{"canonical_adoption_transform_digest",
				 text_value(claims.canonical_adoption_transform_digest())},
				{"base_ingestion_transform_digest",
				 text_value(claims.base_ingestion_transform_digest())},
			});
			if (!direct_basis)
				return sdk::unexpected(std::move(direct_basis.error()));
			return make_object({
				{"selector", std::move(*selector)},
				{"direct_basis", std::move(*direct_basis)},
				{"claim_rows", json_value::array(std::move(row_values))},
				{"claim_envelopes", json_value::array(std::move(envelope_values))},
				{"canonicalization_edges", json_value::array(std::move(edge_values))},
				{"origin_associations", json_value::array(std::move(association_values))},
				{"claim_batch_validation", std::move(*claim_batch)},
				{"partitions", json_value::array(std::move(partition_values))},
				{"snapshot_manifest", std::move(*manifest)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		claim_stages_json(const json_value& task_results,
						  const json_value& store,
						  const std::string_view guarantee_digest)
		{
			const auto* envelopes = store.member("claim_envelopes");
			const auto* associations = store.member("origin_associations");
			if (envelopes == nullptr || !envelopes->as_array() || associations == nullptr ||
				!associations->as_array())
				return sdk::unexpected(
					{"materialization.report-invalid", "claim_stages", "store-shape"});
			json_value::array_type output;
			for (const auto descriptor : task_descriptor_ids)
			{
				std::vector<const json_value*> final;
				for (const auto& envelope : *envelopes->as_array())
				{
					const auto* role = envelope.member("role");
					const auto* id = envelope.member("descriptor_id");
					if (role != nullptr && id != nullptr && role->as_string() != nullptr &&
						id->as_string() != nullptr && *role->as_string() == "stored_final" &&
						*id->as_string() == descriptor)
						final.push_back(&envelope);
				}
				std::ranges::sort(final,
								  [](const auto* left, const auto* right)
								  {
									  return *left->member("claim_ref")->as_string() <
										  *right->member("claim_ref")->as_string();
								  });
				json_value::array_type refs;
				std::set<std::string, std::less<>> content_set;
				std::vector<std::string> provenance_roots;
				std::set<std::string, std::less<>> final_refs;
				for (const auto* envelope : final)
				{
					const auto ref = *envelope->member("claim_ref")->as_string();
					final_refs.insert(ref);
					refs.push_back(text_value(ref));
					content_set.insert(*envelope->member("content")->as_string());
					provenance_roots.push_back(*envelope->member("provenance_root")->as_string());
				}
				json_value::array_type contents;
				for (const auto& content : content_set)
					contents.push_back(text_value(content));
				std::ranges::sort(provenance_roots);
				json_value::array_type provenance_values;
				for (const auto& root : provenance_roots)
					provenance_values.push_back(text_value(root));
				json_value::array_type association_ids;
				for (const auto& association : *associations->as_array())
				{
					const auto* ref = association.member("stored_claim_ref");
					if (ref != nullptr && ref->as_string() != nullptr &&
						final_refs.contains(*ref->as_string()))
						association_ids.push_back(
							text_value(*association.member("association_id")->as_string()));
				}
				std::ranges::sort(association_ids,
								  [](const json_value& left, const json_value& right)
								  {
									  return *left.as_string() < *right.as_string();
								  });
				auto claim_content_set = semantic_projection_digest(
					"cxxlens.clang22-claim-stage-content-set.v1",
					make_object({{"descriptor_id", text_value(descriptor)},
								 {"claim_content_ids", json_value::array(contents)}})
						.value());
				auto occurrence_set = semantic_projection_digest(
					"cxxlens.clang22-claim-stage-sdk-occurrence-set.v1",
					make_object({{"descriptor_id", text_value(descriptor)},
								 {"stored_claim_refs", json_value::array(refs)}})
						.value());
				auto association_set = semantic_projection_digest(
					"cxxlens.clang22-claim-stage-origin-association-set.v1",
					make_object({{"descriptor_id", text_value(descriptor)},
								 {"origin_association_ids", json_value::array(association_ids)}})
						.value());
				auto provenance_set = semantic_projection_digest(
					"cxxlens.clang22-claim-stage-provenance-set.v1",
					make_object({{"descriptor_id", text_value(descriptor)},
								 {"provenance_roots", json_value::array(provenance_values)}})
						.value());
				if (!claim_content_set || !occurrence_set || !association_set || !provenance_set)
					return sdk::unexpected(
						{"materialization.report-invalid", "claim_stages", "digest"});
				object stage{
					{"descriptor_id", text_value(descriptor)},
					{"stage",
					 text_value(is_observation_descriptor(descriptor) ? "assertion"
																	  : "canonical_claim")},
					{"claim_content_ids", json_value::array(contents)},
					{"claim_content_count", json_value::unsigned_integer(content_set.size())},
					{"stored_claim_refs", json_value::array(refs)},
					{"sdk_claim_occurrence_count", json_value::unsigned_integer(final.size())},
					{"origin_association_ids", json_value::array(association_ids)},
					{"origin_association_count",
					 json_value::unsigned_integer(association_ids.size())},
					{"claim_content_set_digest", text_value(*claim_content_set)},
					{"sdk_claim_occurrence_set_digest", text_value(*occurrence_set)},
					{"origin_association_set_digest", text_value(*association_set)},
					{"provenance_edge_set_digest", text_value(*provenance_set)},
					{"guarantee_digest", text_value(guarantee_digest)},
				};
				if (is_observation_descriptor(descriptor))
				{
					auto census = global_observation_census(task_results, descriptor);
					if (!census)
						return sdk::unexpected(std::move(census.error()));
					const auto* exact_count = census->member("exact_equivalence_count");
					const auto* non_exact_count = census->member("non_exact_equivalence_count");
					const auto* rows = census->member("rows");
					const auto* row_digest = census->member("row_equivalence_set_digest");
					if (rows == nullptr || exact_count == nullptr || non_exact_count == nullptr ||
						!rows->as_array() || row_digest == nullptr ||
						row_digest->as_string() == nullptr)
						return sdk::unexpected({"materialization.report-invalid",
												"claim_stages.observation_equivalence_census",
												"projection"});
					auto stage_census = make_object({
						{"rows", *rows},
						{"exact_equivalence_count", *exact_count},
						{"non_exact_equivalence_count", *non_exact_count},
						{"row_equivalence_set_digest", text_value(*row_digest->as_string())},
					});
					if (!stage_census)
						return sdk::unexpected(std::move(stage_census.error()));
					stage.emplace("observation_equivalence_census", std::move(*stage_census));
				}
				auto stage_without_digest = json_value::object(stage);
				if (!stage_without_digest)
					return sdk::unexpected(std::move(stage_without_digest.error()));
				auto stage_digest = semantic_projection_digest("cxxlens.clang22-claim-stage.v1",
															   *stage_without_digest);
				if (!stage_digest)
					return sdk::unexpected(std::move(stage_digest.error()));
				stage.emplace("claim_stage_digest", text_value(*stage_digest));
				output.push_back(json_value::object(std::move(stage)).value());
			}
			return json_value::array(std::move(output));
		}

		[[nodiscard]] sdk::result<json_value> provenance_json(const json_value& claim_stages)
		{
			const auto* stages = claim_stages.as_array();
			if (stages == nullptr || stages->size() != 6U)
				return sdk::unexpected(
					{"materialization.report-invalid", "provenance", "stage-census"});
			std::uint64_t canonical_count{};
			for (const auto& stage : *stages)
			{
				const auto* descriptor = stage.member("descriptor_id");
				const auto* count = stage.member("origin_association_count");
				if (descriptor == nullptr || count == nullptr ||
					descriptor->as_string() == nullptr || count->as_unsigned_integer() == nullptr)
					return sdk::unexpected(
						{"materialization.report-invalid", "provenance", "stage-shape"});
				if (is_canonical_descriptor(*descriptor->as_string()))
					canonical_count += *count->as_unsigned_integer();
			}
			object value{
				{"record_type", text_value("typed-provenance-edge-summary")},
				{"edge_count", json_value::unsigned_integer(canonical_count)},
				{"canonical_claim_count", json_value::unsigned_integer(canonical_count)},
				{"canonical_claims_with_exact_input_edges",
				 json_value::unsigned_integer(canonical_count)},
				{"orphan_count", json_value::unsigned_integer(0U)},
				{"ambiguous_count", json_value::unsigned_integer(0U)},
			};
			json_value::array_type canonical_stages;
			for (const auto descriptor : canonical_task_descriptor_ids)
			{
				const auto found = std::ranges::find_if(
					*stages,
					[&](const json_value& stage)
					{
						return stage.member("descriptor_id") != nullptr &&
							stage.member("descriptor_id")->as_string() != nullptr &&
							*stage.member("descriptor_id")->as_string() == descriptor;
					});
				if (found == stages->end())
					return sdk::unexpected({"materialization.report-invalid",
											"provenance",
											"canonical-stage-missing"});
				canonical_stages.push_back(
					make_object({
									{"descriptor_id", text_value(descriptor)},
									{"sdk_claim_occurrence_count",
									 *found->member("sdk_claim_occurrence_count")},
									{"origin_association_count",
									 *found->member("origin_association_count")},
									{"provenance_edge_set_digest",
									 *found->member("provenance_edge_set_digest")},
									{"claim_stage_digest", *found->member("claim_stage_digest")},
								})
						.value());
			}
			value.emplace("canonical_claim_stages", json_value::array(std::move(canonical_stages)));
			auto digest_projection = json_value::object(value);
			if (!digest_projection)
				return sdk::unexpected(std::move(digest_projection.error()));
			auto digest = semantic_projection_digest("cxxlens.clang22-global-provenance.v1",
													 *digest_projection);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			// The canonical-stage census authenticates the digest but is emitted through
			// the separate `claim_stages` field required by the report schema.
			value.erase("canonical_claim_stages");
			value.emplace("edge_set_digest", text_value(*digest));
			return json_value::object(std::move(value));
		}

		[[nodiscard]] sdk::result<json_value>
		rooted_effect_receipt_json(const materialization_rooted_vfs_receipt& receipt)
		{
			if (receipt.schema != "rooted-vfs-v1" ||
				receipt.mount_device_inode_observation_digest.empty() ||
				receipt.exact_relative_path.empty() || receipt.parent_resolution_verdict.empty() ||
				receipt.leaf_resolution_verdict.empty())
				return sdk::unexpected({"materialization.report-invalid",
										"publication.sqlite_effect_root_receipt",
										"incomplete"});
			if (auto valid = validate_materialization_sqlite_path(receipt.exact_relative_path);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (receipt.parent_resolution_verdict != "openat2-beneath-no-symlinks-no-magiclinks" ||
				receipt.leaf_resolution_verdict != "database-and-sidecars-rooted-no-follow")
				return sdk::unexpected({"materialization.report-invalid",
										"publication.sqlite_effect_root_receipt",
										"resolution-policy"});
			return make_object({
				{"contract", text_value("rooted-vfs-v1")},
				{"root_observation_digest",
				 text_value(receipt.mount_device_inode_observation_digest)},
				{"relative_path", text_value(receipt.exact_relative_path)},
				{"parent_resolution", text_value(receipt.parent_resolution_verdict)},
				{"leaf_resolution", text_value(receipt.leaf_resolution_verdict)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		publication_json(const prevalidated_materialization_request_v2_1& request,
						 const materialization_store_observation& observation,
						 const materialization_rooted_vfs_receipt* rooted_receipt)
		{
			if (!observation.publication_attempted || observation.publish_call_count != 1U ||
				observation.first_issue || !observation.publish_returned_record ||
				!observation.publish_returned_handle || !observation.candidate_identity ||
				!observation.candidate_manifest ||
				observation.backend != request.publication().backend ||
				observation.selector != request.publication().selector ||
				observation.series_id != request.publication().series_id ||
				observation.expected_parent_publication !=
					request.publication().expected_parent_publication)
				return sdk::unexpected(
					{"materialization.report-invalid", "publication", "unverified"});
			const auto& record = *observation.publish_returned_record;
			const auto& handle = *observation.publish_returned_handle;
			if (record.state != sdk::publication_state::committed || record.corrupt ||
				handle.publication() != record ||
				handle.manifest() != *observation.candidate_manifest)
				return sdk::unexpected(
					{"materialization.report-invalid", "publication", "record-mismatch"});
			const auto& head = observation.head_observation;
			const bool head_absent =
				head.status == materialization_store_receipt_status::sdk_error && head.error &&
				head.error->code == "store.current-not-found" && !head.projection && !head.handle;
			const bool head_present =
				head.status == materialization_store_receipt_status::present && head.projection &&
				head.handle && !head.error;
			if (!head_absent && !head_present)
				return sdk::unexpected({"materialization.report-invalid",
										"publication.head_observation",
										"closed-state"});
			if (head_present &&
				(!request.publication().expected_parent_publication ||
				 head.projection->publication.state != sdk::publication_state::committed ||
				 head.projection->publication.corrupt ||
				 head.projection->publication.publication_id !=
					 *request.publication().expected_parent_publication))
				return sdk::unexpected({"materialization.report-invalid",
										"publication.observed_parent_record",
										"parent-mismatch"});
			if (head_absent && request.publication().expected_parent_publication)
				return sdk::unexpected({"materialization.report-invalid",
										"publication.observed_parent_publication",
										"absent-parent"});
			const auto& terminal = observation.verification_receipts[0U];
			if (terminal.status != materialization_store_receipt_status::present ||
				!terminal.projection || !terminal.handle || terminal.error ||
				terminal.projection->publication != record ||
				terminal.projection->manifest != handle.manifest())
				return sdk::unexpected(
					{"materialization.report-invalid", "publication.terminal_head", "not-present"});
			auto selector = selector_fields_json(observation.selector);
			auto candidate = publication_identity_json(record);
			auto committed = publication_record_json(record);
			if (!selector || !candidate || !committed)
				return sdk::unexpected(!selector		? std::move(selector.error())
										   : !candidate ? std::move(candidate.error())
														: std::move(committed.error()));
			json_value observed_parent_record = json_value::null();
			if (head_present)
			{
				auto parent = publication_record_json(head.projection->publication);
				if (!parent)
					return sdk::unexpected(std::move(parent.error()));
				observed_parent_record = std::move(*parent);
			}
			auto terminal_record = publication_record_json(terminal.projection->publication);
			if (!terminal_record)
				return sdk::unexpected(std::move(terminal_record.error()));
			json_value effect = json_value::null();
			if (observation.backend == "sqlite")
			{
				if (rooted_receipt == nullptr)
					return sdk::unexpected({"materialization.report-invalid",
											"publication.sqlite_effect_root_receipt",
											"missing"});
				auto rooted = rooted_effect_receipt_json(*rooted_receipt);
				if (!rooted)
					return sdk::unexpected(std::move(rooted.error()));
				effect = std::move(*rooted);
			}
			return make_object({
				{"backend", text_value(observation.backend)},
				{"selector", std::move(*selector)},
				{"series_id", text_value(observation.series_id)},
				{"genesis", json_value::boolean(request.publication().genesis)},
				{"expected_parent_publication",
				 request.publication().expected_parent_publication
					 ? text_value(*request.publication().expected_parent_publication)
					 : json_value::null()},
				{"observed_parent_publication",
				 head_present ? text_value(head.projection->publication.publication_id)
							  : json_value::null()},
				{"observed_parent_record", std::move(observed_parent_record)},
				{"head_observation", text_value(head_present ? "present" : "absent")},
				{"publication_attempted", json_value::boolean(true)},
				{"outcome", text_value("committed_verified")},
				{"partial_policy", text_value("forbid")},
				{"candidate_snapshot_id", text_value(record.snapshot_id)},
				{"candidate_identity_state", text_value("constructed")},
				{"candidate_identity", std::move(*candidate)},
				{"invocation_commit_state", text_value("committed")},
				{"committed_transaction_count", json_value::unsigned_integer(1U)},
				{"invocation_committed_record", std::move(*committed)},
				{"terminal_head",
				 make_object(
					 {{"status", text_value("present")}, {"record", std::move(*terminal_record)}})
					 .value()},
				{"candidate_visibility", text_value("present_by_invocation")},
				{"prior_history_retained", json_value::boolean(true)},
				{"head_effect", text_value("advanced_to_candidate")},
				{"store_failure", json_value::null()},
				{"sqlite_effect_root_receipt", std::move(effect)},
				{"sqlite_reopen_status",
				 text_value(observation.backend == "sqlite" ? "opened" : "not_applicable")},
				{"recovery_receipt", json_value::null()},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		reopened_annotation_json(const std::string_view descriptor,
								 const sdk::snapshot_claim_annotation& annotation)
		{
			auto presence = claim_condition_json(annotation.presence);
			auto producer = claim_producer_json(annotation.producer);
			auto guarantee = claim_guarantee_json(annotation.guarantee);
			if (!presence || !producer || !guarantee)
				return sdk::unexpected(!presence	   ? std::move(presence.error())
										   : !producer ? std::move(producer.error())
													   : std::move(guarantee.error()));
			return make_object({
				{"relation_descriptor_id", text_value(descriptor)},
				{"row_canonical_form", text_value(annotation.row.canonical_form())},
				{"presence", std::move(*presence)},
				{"interpretation", text_value(annotation.interpretation)},
				{"semantic_key", text_value(annotation.semantic_key)},
				{"assertion", text_value(annotation.assertion)},
				{"content", text_value(annotation.content)},
				{"producer", std::move(*producer)},
				{"provenance_root", text_value(annotation.provenance_root)},
				{"guarantee", std::move(*guarantee)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		reopened_partition_binding_json(const sdk::snapshot_partition_binding& binding)
		{
			auto condition = claim_condition_json(binding.condition);
			if (!condition)
				return sdk::unexpected(std::move(condition.error()));
			return make_object({
				{"partition_id", text_value(binding.partition_id)},
				{"relation_descriptor_id", text_value(binding.relation_descriptor_id)},
				{"scope", text_value(binding.scope)},
				{"condition", std::move(*condition)},
				{"interpretation", text_value(binding.interpretation)},
				{"producer_semantics", text_value(binding.producer_semantics)},
				{"producer_input_basis_digest", text_value(binding.producer_input_basis_digest)},
				{"precision_profile", text_value(binding.precision_profile)},
				{"assumption_set_id", text_value(binding.assumption_set_id)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		reopened_coverage_json(const sdk::snapshot_query_coverage& coverage)
		{
			auto unit = store_coverage_json(coverage.unit);
			if (!unit)
				return sdk::unexpected(std::move(unit.error()));
			return make_object(
				{{"relation_descriptor_id", text_value(coverage.relation_descriptor_id)},
				 {"unit", std::move(*unit)}});
		}

		struct reopened_handle_projection_bundle
		{
			json_value projection{json_value::null()};
			json_value descriptors{json_value::null()};
			json_value partition_bindings{json_value::null()};
			json_value claim_annotations{json_value::null()};
			json_value coverage{json_value::null()};
			json_value relations{json_value::null()};
			json_value cursor{json_value::null()};
			std::string canonical_export_digest;
		};

		[[nodiscard]] sdk::result<reopened_handle_projection_bundle>
		project_reopened_handle(const sdk::snapshot_handle& handle,
								const sdk::relation_engine& engine,
								const json_value& engine_projection,
								const json_value& store_projection,
								const std::string_view canonical_export_digest)
		{
			const auto* admitted = engine_projection.member("admitted_descriptors");
			const auto* snapshot_manifest = store_projection.member("snapshot_manifest");
			if (admitted == nullptr || !admitted->as_array() || snapshot_manifest == nullptr)
				return sdk::unexpected(
					{"materialization.report-invalid", "semantic_verification", "authority-shape"});
			json_value::array_type descriptors = *admitted->as_array();
			std::ranges::sort(descriptors,
							  [](const json_value& left, const json_value& right)
							  {
								  return *left.member("descriptor_id")->as_string() <
									  *right.member("descriptor_id")->as_string();
							  });
			json_value::array_type partition_bindings;
			for (const auto& binding : handle.partition_bindings())
			{
				auto value = reopened_partition_binding_json(binding);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				partition_bindings.push_back(std::move(*value));
			}
			std::ranges::sort(partition_bindings,
							  [](const json_value& left, const json_value& right)
							  {
								  return *left.member("partition_id")->as_string() <
									  *right.member("partition_id")->as_string();
							  });
			json_value::array_type annotations;
			for (const auto& descriptor : descriptors)
			{
				const auto descriptor_id = *descriptor.member("descriptor_id")->as_string();
				auto cursor = handle.open_claims(descriptor_id);
				if (!cursor)
					return sdk::unexpected(std::move(cursor.error()));
				while (true)
				{
					auto next = cursor->next();
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					if (!*next)
						break;
					auto annotation = (*next)->copy();
					if (!annotation)
						return sdk::unexpected(std::move(annotation.error()));
					auto value = reopened_annotation_json(descriptor_id, *annotation);
					if (!value)
						return sdk::unexpected(std::move(value.error()));
					annotations.push_back(std::move(*value));
				}
			}
			std::ranges::sort(annotations,
							  [](const json_value& left, const json_value& right)
							  {
								  return canonical_json(left) < canonical_json(right);
							  });
			json_value::array_type coverage;
			for (const auto& value : handle.input_coverage())
			{
				auto projected = reopened_coverage_json(value);
				if (!projected)
					return sdk::unexpected(std::move(projected.error()));
				coverage.push_back(std::move(*projected));
			}
			std::ranges::sort(coverage,
							  [](const json_value& left, const json_value& right)
							  {
								  return canonical_json(left) < canonical_json(right);
							  });
			if (!handle.unresolved_items().empty() || !handle.closure_certificates().empty() ||
				!handle.manifest().closure_ids.empty())
				return sdk::unexpected({"materialization.report-invalid",
										"semantic_verification",
										"unresolved-or-closure"});

			json_value::array_type relations;
			json_value::array_type coverage_multiset = coverage;
			for (const auto& descriptor : descriptors)
			{
				const auto descriptor_id = *descriptor.member("descriptor_id")->as_string();
				auto relation = engine.require_id(descriptor_id);
				if (!relation)
					return sdk::unexpected(std::move(relation.error()));
				auto cursor = handle.open(*relation);
				if (!cursor)
					return sdk::unexpected(std::move(cursor.error()));
				std::vector<std::string> rows;
				while (true)
				{
					auto next = cursor->next();
					if (!next)
						return sdk::unexpected(std::move(next.error()));
					if (!*next)
						break;
					auto row = (*next)->copy();
					if (!row)
						return sdk::unexpected(std::move(row.error()));
					rows.push_back(row->canonical_form());
				}
				std::ranges::sort(rows);
				rows.erase(std::ranges::unique(rows).begin(), rows.end());
				json_value::array_type row_values;
				for (const auto& row : rows)
					row_values.push_back(text_value(row));
				json_value::array_type relation_annotations;
				for (const auto& annotation : annotations)
					if (*annotation.member("relation_descriptor_id")->as_string() == descriptor_id)
						relation_annotations.push_back(annotation);
				json_value::array_type relation_coverage;
				for (const auto& value : coverage)
					if (*value.member("relation_descriptor_id")->as_string() == descriptor_id)
						relation_coverage.push_back(*value.member("unit"));
				std::ranges::sort(relation_annotations,
								  [](const json_value& left, const json_value& right)
								  {
									  return canonical_json(left) < canonical_json(right);
								  });
				std::ranges::sort(relation_coverage,
								  [](const json_value& left, const json_value& right)
								  {
									  return canonical_json(left) < canonical_json(right);
								  });
				auto row_forms = json_value::array(row_values);
				relations.push_back(
					make_object({
									{"relation_descriptor_id", text_value(descriptor_id)},
									{"row_canonical_forms", row_forms},
									{"claim_annotations",
									 json_value::array(std::move(relation_annotations))},
									{"coverage", json_value::array(std::move(relation_coverage))},
								})
						.value());
			}
			json_value::array_type annotation_multiset = annotations;
			std::ranges::sort(annotation_multiset,
							  [](const json_value& left, const json_value& right)
							  {
								  return canonical_json(left) < canonical_json(right);
							  });
			std::ranges::sort(coverage_multiset,
							  [](const json_value& left, const json_value& right)
							  {
								  return canonical_json(left) < canonical_json(right);
							  });
			auto relations_value = json_value::array(relations);
			json_value::array_type cursor_without_digest;
			cursor_without_digest.push_back(
				text_value("cxxlens.clang22-materialization-reopen-cursor.v1"));
			cursor_without_digest.push_back(relations_value);
			auto cursor_digest = semantic_projection_digest(
				"cxxlens.clang22-materialization-reopen-cursor.v1",
				make_object({{"specification",
							  text_value("cxxlens.clang22-materialization-reopen-cursor.v1")},
							 {"relations", relations_value}})
					.value());
			if (!cursor_digest)
				return sdk::unexpected(std::move(cursor_digest.error()));
			auto cursor = make_object({
				{"specification", text_value("cxxlens.clang22-materialization-reopen-cursor.v1")},
				{"relations", std::move(relations_value)},
				{"digest", text_value(*cursor_digest)},
			});
			if (!cursor)
				return sdk::unexpected(std::move(cursor.error()));

			auto snapshot_manifest_digest = content_digest_text(canonical_json(*snapshot_manifest));
			auto partition_digest =
				semantic_projection_digest("cxxlens.clang22-reopened-partition-binding-multiset.v1",
										   json_value::array(partition_bindings));
			json_value::array_type row_digest_values;
			for (const auto& relation : relations)
				row_digest_values.push_back(json_value::array({
					*relation.member("relation_descriptor_id"),
					*relation.member("row_canonical_forms"),
				}));
			auto row_digest =
				semantic_projection_digest("cxxlens.clang22-reopened-row-multiset.v1",
										   json_value::array(std::move(row_digest_values)));
			auto annotation_digest =
				semantic_projection_digest("cxxlens.clang22-reopened-claim-annotation-multiset.v1",
										   json_value::array(annotation_multiset));
			auto coverage_digest =
				semantic_projection_digest("cxxlens.clang22-reopened-coverage-multiset.v1",
										   json_value::array(coverage_multiset));
			auto unresolved_digest = semantic_projection_digest(
				"cxxlens.clang22-reopened-unresolved.v1", json_value::array({}));
			auto closure_digest = semantic_projection_digest("cxxlens.clang22-reopened-closure.v1",
															 json_value::array({}));
			if (!partition_digest || !row_digest || !annotation_digest || !coverage_digest ||
				!unresolved_digest || !closure_digest)
				return sdk::unexpected(
					{"materialization.report-invalid", "semantic_verification", "digest"});
			auto semantic_fields = make_object({
				{"backend", text_value(handle.physical_backend())},
				{"snapshot_manifest", *snapshot_manifest},
				{"snapshot_manifest_digest", text_value(snapshot_manifest_digest)},
				{"descriptors", json_value::array(descriptors)},
				{"partition_binding_multiset_digest", text_value(*partition_digest)},
				{"row_multiset_digest", text_value(*row_digest)},
				{"claim_annotation_multiset_digest", text_value(*annotation_digest)},
				{"coverage_multiset_digest", text_value(*coverage_digest)},
				{"unresolved_digest", text_value(*unresolved_digest)},
				{"closure_digest", text_value(*closure_digest)},
				{"cursor_projection_digest", text_value(*cursor_digest)},
				{"canonical_export_digest", text_value(canonical_export_digest)},
			});
			if (!semantic_fields)
				return sdk::unexpected(std::move(semantic_fields.error()));
			auto semantic_digest_value = semantic_projection_digest(
				"cxxlens.clang22-reopened-semantic-projection.v1", *semantic_fields);
			if (!semantic_digest_value)
				return sdk::unexpected(std::move(semantic_digest_value.error()));
			auto publication = publication_record_json(handle.publication());
			if (!publication)
				return sdk::unexpected(std::move(publication.error()));
			object projection{
				{"backend", text_value(handle.physical_backend())},
				{"publication_record", *publication},
				{"snapshot_manifest", *snapshot_manifest},
				{"snapshot_manifest_digest", text_value(snapshot_manifest_digest)},
				{"descriptors", json_value::array(descriptors)},
				{"partition_binding_multiset_digest", text_value(*partition_digest)},
				{"row_multiset_digest", text_value(*row_digest)},
				{"claim_annotation_multiset_digest", text_value(*annotation_digest)},
				{"coverage_multiset_digest", text_value(*coverage_digest)},
				{"unresolved_digest", text_value(*unresolved_digest)},
				{"closure_digest", text_value(*closure_digest)},
				{"cursor_projection_digest", text_value(*cursor_digest)},
				{"canonical_export_digest", text_value(canonical_export_digest)},
				{"semantic_projection_digest", text_value(*semantic_digest_value)},
			};
			auto handle_digest =
				semantic_projection_digest("cxxlens.clang22-reopened-handle-projection.v1",
										   json_value::object(projection).value());
			if (!handle_digest)
				return sdk::unexpected(std::move(handle_digest.error()));
			projection.emplace("handle_projection_digest", text_value(*handle_digest));
			return reopened_handle_projection_bundle{
				json_value::object(std::move(projection)).value(),
				json_value::array(std::move(descriptors)),
				json_value::array(std::move(partition_bindings)),
				json_value::array(std::move(annotations)),
				json_value::array(std::move(coverage)),
				json_value::array(std::move(relations)),
				std::move(*cursor),
				std::string{canonical_export_digest}};
		}

		[[nodiscard]] sdk::result<json_value>
		semantic_verification_json(const prevalidated_materialization_request_v2_1& request,
								   const json_value& engine_projection,
								   const json_value& store_projection,
								   const materialization_store_observation& observation)
		{
			if (!observation.verification_store || !observation.publish_returned_record ||
				observation.verification_receipts[0U].handle == std::nullopt ||
				observation.verification_receipts[1U].handle == std::nullopt ||
				observation.verification_receipts[2U].handle == std::nullopt)
				return sdk::unexpected(
					{"materialization.report-invalid", "semantic_verification", "handles-missing"});
			const auto& record = *observation.publish_returned_record;
			auto actual_export =
				observation.verification_store->canonical_export(record.snapshot_id);
			if (!actual_export || actual_export->empty())
				return sdk::unexpected(actual_export
										   ? sdk::error{"materialization.report-invalid",
														"semantic_verification.canonical_export",
														"empty"}
										   : std::move(actual_export.error()));
			std::array<reopened_handle_projection_bundle, 3U> bundles;
			for (std::size_t index{}; index < observation.verification_receipts.size(); ++index)
			{
				const auto& receipt = observation.verification_receipts[index];
				if (receipt.status != materialization_store_receipt_status::present ||
					!receipt.projection || receipt.error || receipt.handle->publication() != record)
					return sdk::unexpected({"materialization.report-invalid",
											"semantic_verification",
											"receipt-mismatch"});
				auto projected = project_reopened_handle(*receipt.handle,
														 request.engine(),
														 engine_projection,
														 store_projection,
														 content_digest_text(*actual_export));
				if (!projected)
					return sdk::unexpected(std::move(projected.error()));
				bundles[index] = std::move(*projected);
				const auto* manifest = bundles[index].projection.member("snapshot_manifest");
				if (manifest == nullptr ||
					*manifest != *store_projection.member("snapshot_manifest"))
					return sdk::unexpected({"materialization.report-invalid",
											"semantic_verification",
											"manifest-mismatch"});
			}
			const auto* first_semantic =
				bundles[0U].projection.member("semantic_projection_digest");
			if (first_semantic == nullptr || first_semantic->as_string() == nullptr)
				return sdk::unexpected(
					{"materialization.report-invalid", "semantic_verification", "semantic-digest"});
			for (const auto& bundle : bundles)
				if (bundle.projection.member("semantic_projection_digest") == nullptr ||
					*bundle.projection.member("semantic_projection_digest") != *first_semantic)
					return sdk::unexpected(
						{"materialization.report-invalid", "semantic_verification", "cross-path"});

			const auto* first_publication = bundles[0U].projection.member("publication_record");
			if (first_publication == nullptr)
				return sdk::unexpected(
					{"materialization.report-invalid", "semantic_verification", "publication"});
			json_value::array_type receipts;
			const std::array<std::string_view, 3U> access_paths{
				"current-selector", "open-publication", "open-snapshot"};
			for (std::size_t index{}; index < access_paths.size(); ++index)
			{
				json_value lookup = json_value::null();
				if (index == 0U)
					lookup =
						make_object({{"selector", *store_projection.member("selector")}}).value();
				else if (index == 1U)
					lookup = make_object({{"publication_id", text_value(record.publication_id)}})
								 .value();
				else
					lookup = make_object({{"snapshot_id", text_value(record.snapshot_id)}}).value();
				receipts.push_back(make_object({
												   {"access_path", text_value(access_paths[index])},
												   {"lookup", std::move(lookup)},
												   {"status", text_value("present")},
												   {"sdk_code", json_value::null()},
												   {"sdk_field", json_value::null()},
												   {"projection", bundles[index].projection},
											   })
									   .value());
			}
			return make_object({
				{"status", text_value("passed")},
				{"reopened_store",
				 make_object(
					 {
						 {"backend", text_value(observation.backend)},
						 {"selector", *store_projection.member("selector")},
						 {"publication_record", *first_publication},
						 {"snapshot_manifest", *bundles[0U].projection.member("snapshot_manifest")},
						 {"descriptors", bundles[0U].descriptors},
						 {"partition_bindings", bundles[0U].partition_bindings},
						 {"claim_annotations", bundles[0U].claim_annotations},
						 {"coverage", bundles[0U].coverage},
						 {"unresolved", json_value::array({})},
						 {"canonical_export_digest",
						  text_value(bundles[0U].canonical_export_digest)},
						 {"cursor_projection", bundles[0U].cursor},
						 {"handle_receipts", json_value::array(std::move(receipts))},
					 })
					 .value()},
				{"reopen_attempt", json_value::null()},
				{"failure", json_value::null()},
			});
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

		[[nodiscard]] sdk::result<json_value>
		authority_digests_json(const materialization_occurrence_receipt& receipt)
		{
			constexpr std::array<std::pair<std::string_view, std::string_view>, 5U> paths{
				{{"schemas/cxxlens_ng_clang22_materialization_contract.yaml",
				  "share/cxxlens/schemas/cxxlens_ng_clang22_materialization_contract.yaml"},
				 {"schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml",
				  "share/cxxlens/schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml"},
				 {"schemas/cxxlens_ng_clang22_materialization_request.schema.yaml",
				  "share/cxxlens/schemas/cxxlens_ng_clang22_materialization_request.schema.yaml"},
				 {"schemas/cxxlens_ng_clang22_materialization_report.schema.yaml",
				  "share/cxxlens/schemas/cxxlens_ng_clang22_materialization_report.schema.yaml"},
				 {"schemas/cxxlens_ng_relation_registry.yaml",
				  "share/cxxlens/schemas/cxxlens_ng_relation_registry.yaml"}}};
			json_value::array_type values;
			values.reserve(paths.size());
			for (const auto [source_path, installed_path] : paths)
			{
				const auto found =
					std::ranges::find(receipt.files,
									  installed_path,
									  [](const auto& file)
									  {
										  return std::string_view{file.authority.path};
									  });
				if (found == receipt.files.end() || found->authority.digest.empty())
					return sdk::unexpected(
						{"materialization.report-invalid", "authority_digests", "missing"});
				values.push_back(
					make_object({
									{"path", string(std::string{source_path}).value()},
									{"digest", string(found->authority.digest).value()},
								})
						.value());
			}
			return json_value::array(std::move(values));
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

	sdk::result<public_materialization_prepublication_projection>
	prepare_public_materialization_prepublication_projection(
		const validated_materialization_request_v2_1& request,
		const raw_input_observation& raw_input,
		const materialization_occurrence_manifest& occurrence_manifest,
		const materialization_occurrence_receipt& occurrence_receipt,
		const std::size_t maximum_report_bytes)
	{
		try
		{
			if (maximum_report_bytes == 0U || raw_input.byte_limit == 0U || !raw_input.complete ||
				raw_input.observed_size_bytes > raw_input.byte_limit ||
				raw_input.observed_prefix_digest.empty())
				return sdk::unexpected(
					{"materialization.report-invalid", "prepublication", "input-boundary"});
			if (occurrence_manifest.occurrence_payload_digest.empty() ||
				occurrence_manifest.inventory_digest.empty() || occurrence_receipt.files.empty() ||
				occurrence_receipt.occurrence_payload_digest !=
					occurrence_manifest.occurrence_payload_digest ||
				occurrence_receipt.inventory_digest != occurrence_manifest.inventory_digest)
				return sdk::unexpected(
					{"materialization.report-invalid", "prepublication", "occurrence-boundary"});

			const auto& identity = request.identity();
			const auto task_count = request.request().task_count();
			std::vector<sdk::canonical_value> binding_fields{
				sdk::canonical_value::from_string(identity.materialization_request_id),
				sdk::canonical_value::from_string(identity.request_digest),
				sdk::canonical_value::from_string(identity.semantic_request_digest),
				sdk::canonical_value::from_string(raw_input.observed_prefix_digest),
				sdk::canonical_value::from_string(std::to_string(raw_input.observed_size_bytes)),
				sdk::canonical_value::from_string(occurrence_manifest.occurrence_payload_digest),
				sdk::canonical_value::from_string(occurrence_manifest.inventory_digest),
				sdk::canonical_value::from_string(std::to_string(task_count)),
				sdk::canonical_value::from_string(std::to_string(maximum_report_bytes))};
			auto binding = sdk::canonical_identity_digest(
				"cxxlens.clang22.prepublication-report.v1", binding_fields);
			if (!binding)
				return sdk::unexpected(std::move(binding.error()));
			return public_materialization_prepublication_projection{
				std::move(*binding),
				identity.request_digest,
				identity.semantic_request_digest,
				occurrence_manifest.inventory_digest,
				task_count,
				maximum_report_bytes};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				{"materialization.report-invalid", "prepublication", "allocation"});
		}
	}

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
		if (input.task_reports != nullptr && input.task_report_spool != nullptr)
			return sdk::unexpected(
				report_error({public_materialization_report_error_kind::invalid_projection,
							  {},
							  "task_reports",
							  "accumulator-and-spool"}));
		const bool has_task_reports =
			input.task_reports != nullptr || input.task_report_spool != nullptr;
		if (input.prepublication == nullptr)
			missing.emplace_back("prepublication_projection");
		if (input.generated_at.empty())
			missing.emplace_back("generated_at");
		for (const auto& [name, _] : required_supplemental)
			if (!input.projections.values.contains(std::string{name}) &&
				!(input.request_globals != nullptr &&
				  (name == "registry" || name == "engine" || name == "interpretation_policy" ||
				   name == "trust_policy")) &&
				!(has_task_reports &&
				  (name == "task_results" || name == "adoption" || name == "span_validation" ||
				   name == "base_claims" || name == "side_channels" || name == "claim_stages" ||
				   name == "provenance")) &&
				!(input.claims != nullptr && input.store != nullptr &&
				  (name == "store" || name == "publication" || name == "semantic_verification")) &&
				!(name == "authority_digests" && input.occurrence_receipt != nullptr))
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
		auto prepublication =
			prepare_public_materialization_prepublication_projection(*input.request,
																	 *input.raw_input,
																	 *input.occurrence_manifest,
																	 *input.occurrence_receipt,
																	 input.maximum_report_bytes);
		if (!prepublication || input.prepublication == nullptr ||
			*prepublication != *input.prepublication)
			return sdk::unexpected(
				report_error({public_materialization_report_error_kind::invalid_projection,
							  {},
							  "prepublication_projection",
							  "recompute-mismatch"}));

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
		std::optional<task_results_projection> task_results;
		std::optional<json_value> span_validation;
		std::optional<json_value> base_claims;
		std::optional<json_value> side_channels;
		std::optional<json_value> derived_store;
		std::optional<json_value> derived_publication;
		std::optional<json_value> derived_claim_stages;
		std::optional<json_value> derived_provenance;
		std::optional<json_value> derived_semantic_verification;
		if (has_task_reports)
		{
			task_report_source source = input.task_reports != nullptr
				? task_report_source{*input.task_reports}
				: task_report_source{*input.task_report_spool};
			auto projected = project_task_results(source, *input.claims, detailed_report_limits{});
			if (!projected)
				return sdk::unexpected(std::move(projected.error()));
			task_results = std::move(*projected);
			auto span = span_validation_json(
				source, input.request->request().engine(), detailed_report_limits{});
			if (!span)
				return sdk::unexpected(std::move(span.error()));
			span_validation = std::move(*span);
			auto channels = side_channels_json(task_results->results, input.request->request());
			if (!channels)
				return sdk::unexpected(std::move(channels.error()));
			side_channels = std::move(*channels);
			const auto* guarantee = side_channels->member("guarantee");
			const auto* guarantee_digest =
				guarantee == nullptr ? nullptr : guarantee->member("digest");
			if (guarantee_digest == nullptr || guarantee_digest->as_string() == nullptr)
				return sdk::unexpected(
					{"materialization.report-invalid", "side_channels.guarantee", "digest"});
			auto base = base_claims_json(source,
										 input.request->request(),
										 *guarantee_digest->as_string(),
										 detailed_report_limits{});
			if (!base)
				return sdk::unexpected(std::move(base.error()));
			base_claims = std::move(*base);
		}
		if (input.claims != nullptr && input.store != nullptr)
		{
			auto store = store_json(request, *input.claims, *input.store);
			if (!store)
				return sdk::unexpected(std::move(store.error()));
			derived_store = std::move(*store);
			auto publication = publication_json(request, *input.store, input.rooted_vfs_receipt);
			if (!publication)
				return sdk::unexpected(std::move(publication.error()));
			derived_publication = std::move(*publication);
			if (!task_results || !side_channels)
				return sdk::unexpected(
					report_error({public_materialization_report_error_kind::missing_authority,
								  {"task_results", "side_channels"},
								  "claim_stages",
								  "task-authority-required"}));
			const auto* guarantee = side_channels->member("guarantee");
			const auto* guarantee_digest =
				guarantee == nullptr ? nullptr : guarantee->member("digest");
			if (guarantee_digest == nullptr || guarantee_digest->as_string() == nullptr)
				return sdk::unexpected(
					{"materialization.report-invalid", "side_channels.guarantee", "digest"});
			auto stages = claim_stages_json(
				task_results->results, *derived_store, *guarantee_digest->as_string());
			if (!stages)
				return sdk::unexpected(std::move(stages.error()));
			derived_claim_stages = std::move(*stages);
			auto provenance = provenance_json(*derived_claim_stages);
			if (!provenance)
				return sdk::unexpected(std::move(provenance.error()));
			derived_provenance = std::move(*provenance);
			if (input.request_globals == nullptr)
				return sdk::unexpected(
					{"materialization.report-invalid", "engine", "global-authority-required"});
			const auto* engine_projection = input.request_globals->root().member("engine");
			if (engine_projection == nullptr || !is_object(engine_projection))
				return sdk::unexpected(
					{"materialization.report-invalid", "engine", "missing-or-not-object"});
			auto semantic = semantic_verification_json(
				request, *engine_projection, *derived_store, *input.store);
			if (!semantic)
				return sdk::unexpected(std::move(semantic.error()));
			derived_semantic_verification = std::move(*semantic);
		}
		else
		{
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
		}
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
		if (task_results)
		{
			fields.emplace("task_results", task_results->results);
			auto adoption = make_object({
				{"boundary", text_value("sealed-materialization-result")},
				{"visibility", text_value("tool-private-immutable")},
				{"state", text_value("sealed")},
				{"partial_policy", text_value("forbid")},
				{"all_tasks_mandatory", json_value::boolean(true)},
				{"all_groups_mandatory", json_value::boolean(true)},
				{"all_batches_mandatory", json_value::boolean(true)},
				{"task_result_set_digest", text_value(task_results->result_set_digest)},
				{"raw_frames",
				 make_object(
					 {
						 {"authority", text_value("diagnostic-only-non-authoritative")},
						 {"retained", json_value::boolean(false)},
						 {"frame_count", json_value::unsigned_integer(task_results->frame_count)},
						 {"frame_set_digest", text_value(task_results->frame_set_digest)},
					 })
					 .value()},
			});
			if (!adoption)
				return sdk::unexpected(std::move(adoption.error()));
			fields.emplace("adoption", std::move(*adoption));
			fields.emplace("span_validation", std::move(*span_validation));
			fields.emplace("base_claims", std::move(*base_claims));
			fields.emplace("side_channels", std::move(*side_channels));
		}
		if (derived_claim_stages)
			fields.emplace("claim_stages", std::move(*derived_claim_stages));
		if (derived_provenance)
			fields.emplace("provenance", std::move(*derived_provenance));
		if (derived_store)
			fields.emplace("store", std::move(*derived_store));
		if (derived_publication)
			fields.emplace("publication", std::move(*derived_publication));
		if (derived_semantic_verification)
			fields.emplace("semantic_verification", std::move(*derived_semantic_verification));
		if (input.request_globals != nullptr)
		{
			const auto& globals = input.request_globals->root();
			const auto copy_member = [&](const std::string_view name,
										 const std::string_view field) -> sdk::result<void>
			{
				const auto* value = member(globals, name);
				if (value == nullptr)
					return sdk::unexpected(
						{"materialization.report-invalid", std::string{field}, "missing"});
				if (!fields.emplace(std::string{name}, *value).second)
					return sdk::unexpected({"materialization.report-invalid",
											std::string{name},
											"duplicate-derived-field"});
				return {};
			};
			const auto* registry = member(globals, "registry");
			if (registry == nullptr || !is_object(registry))
				return sdk::unexpected(
					{"materialization.report-invalid", "registry", "missing-or-not-object"});
			object registry_projection;
			for (const auto name : {std::string_view{"authority_registry_digest"},
									std::string_view{"base_descriptors"},
									std::string_view{"descriptors"}})
			{
				const auto* value = member(*registry, name);
				if (value == nullptr)
					return sdk::unexpected(
						{"materialization.report-invalid", "registry", "missing-member"});
				registry_projection.emplace(std::string{name}, *value);
			}
			auto registry_value = json_value::object(std::move(registry_projection));
			if (!registry_value || !fields.emplace("registry", std::move(*registry_value)).second)
				return sdk::unexpected(
					{"materialization.report-invalid", "registry", "invalid-projection"});
			for (const auto name : {std::string_view{"engine"},
									std::string_view{"interpretation_policy"},
									std::string_view{"trust_policy"}})
			{
				if (auto copied = copy_member(name, name); !copied)
					return sdk::unexpected(std::move(copied.error()));
			}
		}
		auto authority_digests = authority_digests_json(*input.occurrence_receipt);
		if (!authority_digests)
			return sdk::unexpected(std::move(authority_digests.error()));
		if (const auto supplied = input.projections.values.find("authority_digests");
			supplied != input.projections.values.end())
		{
			if (supplied->second != *authority_digests)
				return sdk::unexpected(
					{"materialization.report-invalid", "authority_digests", "authority-mismatch"});
		}
		else
			fields.emplace("authority_digests", std::move(*authority_digests));
		for (const auto& [name, value] : input.projections.values)
		{
			auto found = fields.find(name);
			if (found != fields.end())
			{
				if (found->second != value)
					return sdk::unexpected(
						report_error({public_materialization_report_error_kind::invalid_projection,
									  {},
									  name,
									  "authority-mismatch"}));
				continue;
			}
			fields.emplace(name, value);
		}
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
