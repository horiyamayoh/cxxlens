#include "materialization_writer_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sdk/bounded_store_v6_internal.hpp"
#include "sdk/bounded_store_v6_memory_internal.hpp"
#include "sdk/bounded_store_v6_sqlite_internal.hpp"
#include "sdk/materialization_store_candidate_bridge_internal.hpp"
#include "sdk/store_backend_lifetime_internal.hpp"
#include "sdk/store_operation_port_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {"materialization.writer-source-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error mismatch(std::string field, std::string detail = {})
		{
			return {"materialization.writer-source-mismatch", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<void> strong(std::string_view value, std::string field)
		{
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return sdk::unexpected(invalid(std::move(field), "strong-id"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_authority(const sdk::relation_engine& engine,
						   const materialization_publication_requirement& authority)
		{
			const std::array<std::pair<std::string_view, std::string_view>, 3U> publication_ids{{
				{"analysis-recipe", authority.analysis_recipe_digest},
				{"output-plan", authority.output_plan_digest},
				{"publication-target", authority.publication_target},
			}};
			for (const auto& [field, value] : publication_ids)
				if (auto valid = strong(value, "authority.publication." + std::string{field});
					!valid)
					return valid;

			if (auto valid = authority.snapshot.series.validate(); !valid)
				return sdk::unexpected(invalid("authority.snapshot.series", valid.error().code));
			if (auto valid = strong(authority.snapshot.catalog_semantic_digest,
									"authority.snapshot.catalog-semantic-digest");
				!valid)
				return valid;
			if (authority.snapshot.expected_parent_publication)
				if (auto valid = strong(*authority.snapshot.expected_parent_publication,
										"authority.snapshot.expected-parent");
					!valid)
					return valid;
			if (authority.snapshot.series.engine_generation_id != engine.generation())
				return sdk::unexpected(
					mismatch("authority.snapshot.series.engine-generation", "engine"));
			if (authority.snapshot.series.relation_registry_digest != engine.registry_digest())
				return sdk::unexpected(
					mismatch("authority.snapshot.series.registry-digest", "engine"));
			return {};
		}

		[[nodiscard]] bounded_store_record_kind projection_kind(const std::string_view kind)
		{
			if (kind == "manifest")
				return bounded_store_record_kind::global_identity;
			if (kind == "partition-manifest")
				return bounded_store_record_kind::partition_census;
			if (kind == "partition-binding")
				return bounded_store_record_kind::partition_begin;
			if (kind == "partition-envelope")
				return bounded_store_record_kind::claim_full_projection;
			if (kind == "closure")
				return bounded_store_record_kind::closure_binding;
			return bounded_store_record_kind::unresolved;
		}

		[[nodiscard]] sdk::result<std::vector<bounded_store_record>>
		make_expected_candidate_records(const validated_materialization_publication_source& source)
		{
			std::vector<sdk::detail::snapshot_candidate_projection_record> records;
			const auto add = [&](std::string kind, std::string key, std::vector<std::byte> payload)
			{
				records.push_back({std::move(kind), std::move(key), std::move(payload)});
			};
			sdk::snapshot_manifest manifest;
			manifest.snapshot_semantics_version =
				source.authority().snapshot.snapshot_semantics_version;
			manifest.catalog_semantic_digest = source.authority().snapshot.catalog_semantic_digest;
			manifest.condition_universe_id =
				source.authority().snapshot.series.condition_universe_id;
			manifest.relation_registry_digest =
				source.authority().snapshot.series.relation_registry_digest;
			manifest.interpretation_policy_digest =
				source.authority().snapshot.series.interpretation_policy_digest;
			auto manifest_payload = sdk::detail::encode_snapshot_candidate_manifest(manifest);
			if (!manifest_payload)
				return sdk::unexpected(std::move(manifest_payload.error()));
			add("manifest", "semantic", std::move(*manifest_payload));
			for (const auto& partition : source.partitions())
			{
				auto payload = sdk::canonical_binary(sdk::canonical_value::from_tuple(
					{sdk::canonical_value::from_string(partition.manifest.partition_id),
					 sdk::canonical_value::from_string(partition.manifest.relation_descriptor_id),
					 sdk::canonical_value::from_string(partition.manifest.input_basis_digest),
					 sdk::canonical_value::from_string(partition.manifest.claim_set_digest),
					 sdk::canonical_value::from_string(partition.manifest.coverage_digest),
					 sdk::canonical_value::from_string(partition.manifest.content_digest),
					 sdk::canonical_value::from_integer(
						 static_cast<std::int64_t>(partition.manifest.claim_count)),
					 sdk::canonical_value::from_boolean(partition.manifest.complete)}));
				if (!payload)
					return sdk::unexpected(std::move(payload.error()));
				add("partition-manifest", partition.manifest.partition_id, std::move(*payload));
				auto binding = sdk::detail::encode_snapshot_candidate_binding(partition.binding);
				if (!binding)
					return sdk::unexpected(std::move(binding.error()));
				add("partition-binding", partition.binding.partition_id, std::move(*binding));
				auto envelope = sdk::detail::encode_snapshot_candidate_partition(partition.draft);
				if (!envelope)
					return sdk::unexpected(std::move(envelope.error()));
				add("partition-envelope", partition.manifest.partition_id, std::move(*envelope));
				for (const auto& unresolved : partition.draft.unresolved)
				{
					auto item = sdk::detail::encode_snapshot_candidate_unresolved(unresolved);
					if (!item)
						return sdk::unexpected(std::move(item.error()));
					add("unresolved", unresolved.source_assertion, std::move(*item));
				}
			}
			for (const auto& closure : source.closures())
			{
				auto payload = sdk::detail::encode_snapshot_candidate_closure(closure);
				if (!payload)
					return sdk::unexpected(std::move(payload.error()));
				add("closure", closure.subject_partition_id, std::move(*payload));
			}
			std::ranges::sort(records,
							  [](const auto& left, const auto& right)
							  {
								  if (left.kind != right.kind)
									  return left.kind < right.kind;
								  if (left.key != right.key)
									  return left.key < right.key;
								  return std::lexicographical_compare(left.payload.begin(),
																	  left.payload.end(),
																	  right.payload.begin(),
																	  right.payload.end());
							  });
			std::vector<bounded_store_record> output;
			output.reserve(records.size());
			for (auto& record : records)
				output.emplace_back(bounded_store_record{projection_kind(record.kind),
														 std::move(record.key),
														 std::move(record.payload)});
			return output;
		}

		sdk::result<void>
		append_candidate_records(bounded_store_record_spool& spool,
								 const std::span<const bounded_store_record> records)
		{
			std::vector<bounded_store_record> ordered{records.begin(), records.end()};
			std::ranges::sort(
				ordered,
				[](const bounded_store_record& left, const bounded_store_record& right)
				{
					const auto left_kind = static_cast<std::uint8_t>(left.kind);
					const auto right_kind = static_cast<std::uint8_t>(right.kind);
					if (left_kind != right_kind)
						return left_kind < right_kind;
					if (left.key != right.key)
						return left.key < right.key;
					return std::lexicographical_compare(left.payload.begin(),
														left.payload.end(),
														right.payload.begin(),
														right.payload.end());
				});
			for (const auto& record : ordered)
				if (auto appended = spool.append(record); !appended)
					return appended;
			return spool.seal();
		}

		[[nodiscard]] sdk::result<std::array<std::byte, 32U>>
		v6_raw_digest(const std::string_view digest)
		{
			if (!digest.starts_with("sha256:") || digest.size() != 71U)
				return sdk::unexpected(invalid("v6.digest", "non-canonical"));
			const auto nibble = [](const char value) -> std::optional<unsigned>
			{
				if (value >= '0' && value <= '9')
					return static_cast<unsigned>(value - '0');
				if (value >= 'a' && value <= 'f')
					return static_cast<unsigned>(value - 'a' + 10);
				return std::nullopt;
			};
			std::array<std::byte, 32U> output{};
			for (std::size_t index{}; index < output.size(); ++index)
			{
				auto high = nibble(digest[7U + index * 2U]);
				auto low = nibble(digest[8U + index * 2U]);
				if (!high || !low)
					return sdk::unexpected(invalid("v6.digest", "hex"));
				output[index] = static_cast<std::byte>((*high << 4U) | *low);
			}
			return output;
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		v6_text_tuple(const std::string_view value)
		{
			return sdk::canonical_binary(sdk::canonical_value::from_tuple(
				{sdk::canonical_value::from_string(std::string{value})}));
		}

		[[nodiscard]] sdk::result<std::vector<sdk::detail::bounded_store_v6_memory_source_frame>>
		make_v6_frames(const sdk::snapshot_series_selector& selector,
					   const std::span<const bounded_store_record> records)
		{
			if (records.empty())
				return sdk::unexpected(invalid("v6.records", "empty"));
			std::vector<bounded_store_record> ordered{records.begin(), records.end()};
			std::ranges::sort(
				ordered,
				[](const bounded_store_record& left, const bounded_store_record& right)
				{
					if (left.kind != right.kind)
						return left.kind < right.kind;
					if (left.key != right.key)
						return left.key < right.key;
					return std::lexicographical_compare(left.payload.begin(),
														left.payload.end(),
														right.payload.begin(),
														right.payload.end());
				});

			auto boundary_key = v6_text_tuple("materialization:" + selector.id());
			if (!boundary_key)
				return sdk::unexpected(std::move(boundary_key.error()));
			auto boundary_payload = v6_text_tuple("candidate");
			if (!boundary_payload)
				return sdk::unexpected(std::move(boundary_payload.error()));
			std::vector<sdk::detail::bounded_store_v6_memory_source_frame> frames;
			frames.reserve(ordered.size() + 2U);
			struct claim_frame
			{
				std::vector<std::byte> key;
				std::vector<std::byte> payload;
				std::vector<std::byte> order;
			};
			std::vector<claim_frame> claims;
			claims.reserve(ordered.size());
			const auto add = [&](const sdk::detail::bounded_store_v6_record_kind kind,
								 const std::vector<std::byte>& key,
								 const std::vector<std::byte>& payload)
			{
				auto encoded =
					sdk::detail::encode_bounded_store_v6_memory_frame(kind, key, payload);
				if (!encoded)
					return sdk::result<void>{sdk::unexpected(std::move(encoded.error()))};
				frames.push_back({kind, std::move(*encoded)});
				return sdk::result<void>{};
			};
			if (auto added = add(sdk::detail::bounded_store_v6_record_kind::partition_begin,
								 *boundary_key,
								 *boundary_payload);
				!added)
				return sdk::unexpected(std::move(added.error()));
			for (std::size_t index{}; index < ordered.size(); ++index)
			{
				const auto& record = ordered[index];
				auto decoded = sdk::canonical_binary_decode(record.payload);
				if (!decoded || decoded->type != sdk::canonical_value::kind::ordered_tuple)
					return sdk::unexpected(invalid("v6.records", "payload-not-canonical-tuple"));
				const auto key_text = std::to_string(static_cast<unsigned>(record.kind)) + ":" +
					record.key + ":" + std::to_string(index);
				auto key = v6_text_tuple(key_text);
				if (!key)
					return sdk::unexpected(std::move(key.error()));
				auto order = sdk::canonical_binary(sdk::canonical_value::from_tuple(
					{sdk::canonical_value::from_bytes(std::vector<std::byte>{static_cast<std::byte>(
						 sdk::detail::bounded_store_v6_record_kind::claim_occurrence)}),
					 sdk::canonical_value::from_bytes(*key),
					 sdk::canonical_value::from_bytes(record.payload)}));
				if (!order)
					return sdk::unexpected(std::move(order.error()));
				claims.push_back({std::move(*key), record.payload, std::move(*order)});
			}
			std::ranges::sort(claims,
							  [](const claim_frame& left, const claim_frame& right)
							  {
								  return std::lexicographical_compare(left.order.begin(),
																	  left.order.end(),
																	  right.order.begin(),
																	  right.order.end());
							  });
			for (const auto& claim : claims)
				if (auto added = add(sdk::detail::bounded_store_v6_record_kind::claim_occurrence,
									 claim.key,
									 claim.payload);
					!added)
					return sdk::unexpected(std::move(added.error()));
			if (auto added = add(sdk::detail::bounded_store_v6_record_kind::partition_end,
								 *boundary_key,
								 *boundary_payload);
				!added)
				return sdk::unexpected(std::move(added.error()));
			return frames;
		}

		class v6_preflight_report_writer final
			: public sdk::detail::bounded_store_report_tail_writer
		{
		  public:
			sdk::result<sdk::detail::bounded_store_v6_report_tail_reservation>
			reserve_maximum_tail(const std::uint64_t tail, const std::uint64_t maximum) override
			{
				if (tail != sdk::detail::bounded_store_v6_exact_report_tail_bytes ||
					maximum != sdk::detail::bounded_store_v6_max_report_bytes)
					return sdk::unexpected(invalid("v6.report", "reservation"));
				reserved_ = true;
				return sdk::detail::bounded_store_v6_report_tail_reservation{
					"materializer-v6-report-writer",
					"materializer-v6-report-spool",
					"materializer-v6-report-reservation",
					0U,
					tail,
					tail,
					maximum};
			}
			sdk::result<void> append_terminal(
				const sdk::detail::bounded_store_v6_publication_terminal terminal) override
			{
				if (!reserved_)
					return sdk::unexpected(invalid("v6.report", "terminal-before-reservation"));
				terminal_ = terminal;
				return {};
			}
			sdk::result<void> validate_full_schema() override
			{
				return terminal_ ? sdk::result<void>{}
								 : sdk::unexpected(invalid("v6.report", "schema"));
			}
			sdk::result<void> validate_complete_section_census(const std::uint32_t count) override
			{
				if (!terminal_ || count != sdk::detail::bounded_store_v6_report_section_count)
					return sdk::unexpected(invalid("v6.report", "section-census"));
				return {};
			}
			sdk::result<void> validate_bottom_up_bindings() override
			{
				return terminal_ ? sdk::result<void>{}
								 : sdk::unexpected(invalid("v6.report", "bindings"));
			}
			sdk::result<std::uint64_t> sealed_report_bytes() const override
			{
				if (!reserved_ || !terminal_)
					return sdk::unexpected(invalid("v6.report", "not-sealed"));
				return sdk::detail::bounded_store_v6_exact_report_tail_bytes;
			}
			sdk::result<void> release() override
			{
				if (!reserved_ || released_)
					return sdk::unexpected(invalid("v6.report", "release"));
				released_ = true;
				return {};
			}

		  private:
			bool reserved_{};
			bool released_{};
			std::optional<sdk::detail::bounded_store_v6_publication_terminal> terminal_;
		};

		[[nodiscard]] sdk::result<void>
		run_v6_candidate_preflight(const sdk::snapshot_series_selector& selector,
								   const std::string_view engine_generation,
								   const std::span<const bounded_store_record> expected_records,
								   const std::span<const bounded_store_record> actual_records,
								   const std::string_view authority_binding,
								   const std::optional<std::string_view> sqlite_path = std::nullopt)
		{
			auto expected_frames = make_v6_frames(selector, expected_records);
			if (!expected_frames)
				return sdk::unexpected(std::move(expected_frames.error()));
			auto actual_frames = make_v6_frames(selector, actual_records);
			if (!actual_frames)
				return sdk::unexpected(std::move(actual_frames.error()));
			std::vector<std::byte> actual_bytes;
			for (const auto& frame : *actual_frames)
				actual_bytes.insert(actual_bytes.end(), frame.bytes.begin(), frame.bytes.end());
			auto binary_digest = v6_raw_digest(sdk::content_digest(actual_bytes));
			if (!binary_digest)
				return sdk::unexpected(std::move(binary_digest.error()));
			const auto event_count = static_cast<std::uint64_t>(actual_frames->size());
			const auto claim_count = event_count - 2U;
			sdk::detail::bounded_store_v6_external_census census{
				1U,
				1U,
				event_count,
				claim_count,
				0U,
				0U,
				0U,
				0U,
				static_cast<std::uint64_t>(actual_bytes.size()),
				*binary_digest,
				std::string{"materializer-v6-authority:"} + std::string{authority_binding}};
			sdk::detail::bounded_store_v6_task_receipt receipt{
				std::string{"materializer-v6-task:"} + std::string{authority_binding},
				0U,
				1U,
				event_count,
				claim_count,
				0U,
				0U,
				0U,
				0U,
				static_cast<std::uint64_t>(actual_bytes.size()),
				*binary_digest,
				std::string{"materializer-v6-task-authority:"} + std::string{authority_binding}};
			auto head = sdk::detail::make_bounded_store_v6_genesis_head(selector);
			if (!head)
				return sdk::unexpected(std::move(head.error()));
			const auto backend_kind = sqlite_path ? sdk::detail::bounded_store_v6_backend::sqlite
												  : sdk::detail::bounded_store_v6_backend::memory;
			sdk::detail::bounded_store_v6_session_metadata metadata{
				backend_kind,
				std::string{engine_generation},
				selector,
				std::string{"materializer-v6-candidate:"} + std::string{authority_binding},
				sqlite_path ? std::optional<std::string>{std::string{*sqlite_path}} : std::nullopt,
				*head};
			const auto session_metadata = metadata;
			std::unique_ptr<sdk::detail::bounded_store_v6_backend_port> backend;
			std::optional<sdk::detail::bounded_store_v6_memory_store> memory_store;
			if (sqlite_path)
			{
				auto sqlite_backend =
					sdk::detail::make_bounded_store_v6_sqlite_backend_port(std::move(metadata));
				if (!sqlite_backend)
					return sdk::unexpected(std::move(sqlite_backend.error()));
				backend = std::move(*sqlite_backend);
			}
			else
			{
				memory_store.emplace(selector, sdk::make_default_store_operation_port());
				auto memory_backend = memory_store->make_backend_port(metadata);
				if (!memory_backend)
					return sdk::unexpected(std::move(memory_backend.error()));
				backend = std::move(*memory_backend);
			}
			auto session = sdk::detail::bounded_store_v6_phase_core::begin_staging_session(
				std::move(session_metadata));
			if (!session)
				return sdk::unexpected(std::move(session.error()));
			auto input = sdk::detail::bounded_store_v6_phase_core::seal_input(std::move(*session),
																			  std::move(census));
			if (!input)
				return sdk::unexpected(std::move(input.error()));
			auto prepared = sdk::detail::bounded_store_v6_phase_core::prepare_publication(
				std::move(*input), std::move(backend));
			if (!prepared)
				return sdk::unexpected(std::move(prepared.error()));
			auto actual_source = sdk::detail::make_bounded_store_v6_memory_task_frame_source(
				std::move(*actual_frames));
			if (!actual_source)
				return sdk::unexpected(std::move(actual_source.error()));
			auto sealed_task = sdk::detail::bounded_store_v6_phase_core::seal_task_source(
				std::move(receipt), std::move(*actual_source));
			if (!sealed_task)
				return sdk::unexpected(std::move(sealed_task.error()));
			if (auto staged = sdk::detail::bounded_store_v6_phase_core::stage_from_source(
					*prepared, std::move(*sealed_task));
				!staged)
				return sdk::unexpected(std::move(staged.error()));
			if (auto sealed =
					sdk::detail::bounded_store_v6_phase_core::seal_prepared_publication(*prepared);
				!sealed)
				return sdk::unexpected(std::move(sealed.error()));
			auto expected_source =
				sdk::detail::make_bounded_store_v6_memory_expected_semantic_source(
					std::move(*expected_frames));
			if (!expected_source)
				return sdk::unexpected(std::move(expected_source.error()));
			auto expected = sdk::detail::bounded_store_v6_phase_core::seal_expected_projection(
				std::move(*expected_source), *prepared);
			if (!expected)
				return sdk::unexpected(std::move(expected.error()));
			auto actual =
				sdk::detail::bounded_store_v6_phase_core::take_physical_actual_cursor(*prepared);
			if (!actual)
				return sdk::unexpected(std::move(actual.error()));
			auto match =
				sdk::detail::bounded_store_v6_phase_core::compare_bounded_store_projections(
					*prepared, std::move(*expected), std::move(*actual));
			if (!match)
				return sdk::unexpected(std::move(match.error()));
			auto report = sdk::detail::bounded_store_v6_phase_core::reserve_report_tail(
				*prepared, *match, std::make_unique<v6_preflight_report_writer>());
			if (!report)
				return sdk::unexpected(std::move(report.error()));
			auto terminal = sdk::detail::bounded_store_v6_phase_core::capture_not_attempted(
				std::move(*prepared), std::move(*report));
			if (!terminal)
				return sdk::unexpected(std::move(terminal.error()));
			auto release =
				sdk::detail::bounded_store_v6_phase_core::finalize_and_validate_report(*terminal);
			if (!release)
				return sdk::unexpected(std::move(release.error()));
			auto drained =
				sdk::detail::bounded_store_v6_phase_core::drain(*terminal, std::move(*release));
			if (!drained || !drained->drained || !drained->report_released ||
				!drained->backend_cleanup_drained)
				return drained ? sdk::unexpected(invalid("v6.cleanup", "not-drained"))
							   : sdk::unexpected(std::move(drained.error()));
			return {};
		}
	} // namespace

	sdk::result<void>
	validated_materialization_publication_source::validate(const sdk::relation_engine& engine) const
	{
		if (auto valid = validate_authority(engine, authority_); !valid)
			return valid;
		for (const auto& [field, value] :
			 {std::pair{std::string_view{"materialization-request-id"},
						std::string_view{materialization_request_id_}},
			  std::pair{std::string_view{"task-id"}, std::string_view{task_id_}},
			  std::pair{std::string_view{"task-input-digest"},
						std::string_view{task_input_digest_}},
			  std::pair{std::string_view{"result-digest"}, std::string_view{result_digest_}},
			  std::pair{std::string_view{"source-receipt-digest"},
						std::string_view{source_receipt_digest_}}})
			if (auto valid = strong(value, std::string{field}); !valid)
				return valid;
		if (terminal_ != materialization_terminal::complete &&
			terminal_ != materialization_terminal::partial)
			return sdk::unexpected(invalid("terminal", "not-publishable"));
		if (partitions_.empty())
			return sdk::unexpected(invalid("partitions", "empty"));

		std::set<std::string, std::less<>> partition_ids;
		for (const auto& value : partitions_)
		{
			if (!partition_ids.insert(value.manifest.partition_id).second)
				return sdk::unexpected(mismatch("partition.partition-id", "duplicate"));
			if (value.draft.condition.universe != authority_.snapshot.series.condition_universe_id)
				return sdk::unexpected(mismatch("partition.condition-universe", "snapshot"));
			auto manifest = sdk::make_partition_manifest(engine, value.draft);
			if (!manifest)
				return sdk::unexpected(std::move(manifest.error()));
			if (*manifest != value.manifest)
				return sdk::unexpected(mismatch("partition.manifest", "recomputed"));
			auto subject = sdk::make_partition_certificate_subject(value.manifest, value.binding);
			if (!subject)
				return sdk::unexpected(std::move(subject.error()));
		}

		std::set<std::string, std::less<>> closure_ids;
		for (const auto& closure : closures_)
		{
			const auto partition = std::ranges::find(partitions_,
													 closure.subject_partition_id,
													 [](const auto& value)
													 {
														 return value.manifest.partition_id;
													 });
			if (partition == partitions_.end())
				return sdk::unexpected(mismatch("closure.subject-partition", "missing"));
			auto subject =
				sdk::make_partition_certificate_subject(partition->manifest, partition->binding);
			if (!subject)
				return sdk::unexpected(std::move(subject.error()));
			auto certificate = sdk::make_closure_certificate(*subject, closure);
			if (!certificate)
				return sdk::unexpected(std::move(certificate.error()));
			if (!closure_ids.insert(certificate->id).second)
				return sdk::unexpected(mismatch("closure.id", "duplicate"));
		}
		return {};
	}

	sdk::result<validated_materialization_publication_source>
	make_materialization_publication_source(
		const sdk::relation_engine& engine,
		const validated_materialization_task& task,
		const validated_materialization_result& result,
		const std::span<const sdk::partition_draft> host_partitions,
		std::string source_receipt_digest)
	{
		if (result.task_id() != task.id() ||
			result.task_input_digest() != task.input_binding_digest())
			return sdk::unexpected(mismatch("result", "task-binding"));
		if (result.terminal() != materialization_terminal::complete &&
			result.terminal() != materialization_terminal::partial)
			return sdk::unexpected(invalid("result.terminal", "not-publishable"));
		if (auto valid = strong(source_receipt_digest, "source-receipt-digest"); !valid)
			return sdk::unexpected(std::move(valid.error()));

		std::vector<materialization_writer_partition> partitions;
		partitions.reserve(host_partitions.size() + result.partitions().size());
		std::set<std::string, std::less<>> relation_ids;
		const auto append = [&](const sdk::partition_draft& draft,
								const sdk::partition_manifest& manifest,
								const sdk::snapshot_partition_binding& binding) -> sdk::result<void>
		{
			if (!relation_ids.insert(draft.relation_descriptor_id).second)
				return sdk::unexpected(mismatch("partition.relation", "duplicate"));
			partitions.push_back({draft, manifest, binding});
			return {};
		};
		for (const auto& draft : host_partitions)
		{
			if (draft.claims.empty())
				return sdk::unexpected(invalid("host-partition", "empty"));
			auto manifest = sdk::make_partition_manifest(engine, draft);
			if (!manifest)
				return sdk::unexpected(std::move(manifest.error()));
			sdk::snapshot_partition_binding binding{manifest->partition_id,
													draft.relation_descriptor_id,
													draft.scope,
													draft.condition,
													draft.interpretation,
													draft.producer_semantics,
													draft.producer_input_basis_digest,
													draft.precision_profile,
													draft.assumption_set_id};
			if (auto added = append(draft, *manifest, binding); !added)
				return sdk::unexpected(std::move(added.error()));
		}
		for (const auto& partition : result.partitions())
			if (auto added = append(partition.draft, partition.manifest, partition.binding); !added)
				return sdk::unexpected(std::move(added.error()));

		validated_materialization_publication_source source{
			task.value().publication,
			task.value().materialization_request_id,
			std::string{result.task_id()},
			std::string{result.task_input_digest()},
			std::string{result.result_digest()},
			std::move(source_receipt_digest),
			result.terminal(),
			std::move(partitions),
			std::vector<sdk::closure_candidate>{result.closures().begin(),
												result.closures().end()}};
		if (auto valid = source.validate(engine); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return source;
	}

	sdk::result<materialization_store_publication>
	publish_materialization_source(const sdk::relation_engine& engine,
								   sdk::snapshot_store& store,
								   validated_materialization_publication_source source,
								   std::optional<std::string> v6_sqlite_path)
	{
		if (auto valid = source.validate(engine); !valid)
			return sdk::unexpected(std::move(valid.error()));
		const auto output_authority = source.authority_;
		const auto materialization_request_id = source.materialization_request_id_;
		const auto task_id = source.task_id_;
		const auto task_input_digest = source.task_input_digest_;
		const auto result_digest = source.result_digest_;
		const auto source_receipt_digest = source.source_receipt_digest_;
		const auto source_terminal = source.terminal_;
		auto expected_records = make_expected_candidate_records(source);
		if (!expected_records)
			return sdk::unexpected(std::move(expected_records.error()));

		auto writer = store.begin(std::move(source.authority_.snapshot));
		if (!writer)
			return sdk::unexpected(std::move(writer.error()));
		for (auto& partition : source.partitions_)
		{
			if (auto staged = writer->stage(std::move(partition.draft)); !staged)
				return sdk::unexpected(std::move(staged.error()));
		}
		for (auto& closure : source.closures_)
		{
			if (auto staged = writer->add_closure(std::move(closure)); !staged)
				return sdk::unexpected(std::move(staged.error()));
		}
		if (auto valid = writer->validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		// Run the bounded v6 phase core against a value-owned expected projection and the
		// backend-derived physical candidate before the ordinary Store publication.  This is a
		// prepublication gate: this writer remains the sole durable effect, while v6 supplies
		// the independent cursor comparison, resource accounting, and one-shot cleanup boundary.
		auto actual_projection =
			sdk::snapshot_store_backend_lifetime_access::candidate_projection(*writer);
		if (!actual_projection)
			return sdk::unexpected(std::move(actual_projection.error()));
		std::vector<bounded_store_record> actual_records;
		actual_records.reserve(actual_projection->records.size());
		for (auto& record : actual_projection->records)
			actual_records.emplace_back(bounded_store_record{
				projection_kind(record.kind), std::move(record.key), std::move(record.payload)});
		const auto expected_for_v6 = *expected_records;
		const auto expected_for_postpublish = expected_for_v6;
		if (auto preflight =
				run_v6_candidate_preflight(output_authority.snapshot.series,
										   output_authority.snapshot.series.engine_generation_id,
										   std::span<const bounded_store_record>{expected_for_v6},
										   std::span<const bounded_store_record>{actual_records},
										   result_digest);
			!preflight)
		{
			writer->cancel();
			return sdk::unexpected(std::move(preflight.error()));
		}
		auto task_payload = sdk::canonical_binary(sdk::canonical_value::from_tuple({
			sdk::canonical_value::from_string(task_id),
			sdk::canonical_value::from_string(task_input_digest),
			sdk::canonical_value::from_string(result_digest),
			sdk::canonical_value::from_string(source_receipt_digest),
		}));
		if (!task_payload)
			return sdk::unexpected(std::move(task_payload.error()));
		// The candidate census covers logical task bytes, while its digest covers the exact
		// bounded-store input frame (kind/key/payload plus the frame checksum).  Keeping those
		// authorities separate is what lets the candidate reject a transport framing change
		// without confusing it with a change to the sealed task payload itself.
		const bounded_store_record task_record{
			bounded_store_record_kind::task_result, "0", *task_payload};
		auto encoded_task_record = encode_bounded_store_record(task_record);
		if (!encoded_task_record)
			return sdk::unexpected(std::move(encoded_task_record.error()));
		const auto input_frame_digest = sdk::content_digest(*encoded_task_record);
		std::optional<sdk::snapshot_handle> published;
		std::optional<sdk::error> publish_error;
		materialization_store_candidate_bridge_request bridge;
		bridge.staging_session_id = "materialization-candidate:" + materialization_request_id;
		bridge.expected_head = output_authority.snapshot.expected_parent_publication.value_or(
			"genesis:" + output_authority.snapshot.series.id());
		bridge.external_census = {
			1U, static_cast<std::uint64_t>(task_payload->size()), input_frame_digest};
		bridge.replay_tasks =
			[task_payload = std::move(*task_payload)](const auto& consume) -> sdk::result<void>
		{
			return consume(task_payload);
		};
		bridge.build_expected_projection =
			[expected = std::move(*expected_records)](
				bounded_store_record_spool& spool) mutable -> sdk::result<void>
		{
			return append_candidate_records(spool, expected);
		};
		bridge.build_actual_projection =
			[&writer](bounded_store_record_spool& spool) -> sdk::result<void>
		{
			auto projection =
				sdk::snapshot_store_backend_lifetime_access::candidate_projection(*writer);
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			std::vector<bounded_store_record> records;
			records.reserve(projection->records.size());
			for (auto& record : projection->records)
				records.emplace_back(bounded_store_record{projection_kind(record.kind),
														  std::move(record.key),
														  std::move(record.payload)});
			return append_candidate_records(spool, records);
		};
		bridge.write_publication_independent_report =
			[](bounded_store_report_writer& report) -> sdk::result<void>
		{
			constexpr std::string_view prefix{"store-candidate:v1"};
			return report.append(std::as_bytes(std::span{prefix.data(), prefix.size()}));
		};
		bridge.write_exact_outcome_report =
			[](bounded_store_report_writer& report,
			   const bounded_store_publication_terminal publication_terminal) -> sdk::result<void>
		{
			const auto value = std::to_string(static_cast<unsigned>(publication_terminal));
			return report.append(std::as_bytes(std::span{value.data(), value.size()}));
		};
		bridge.cleanup = [&writer]() -> sdk::result<void>
		{
			writer->cancel();
			return {};
		};
		std::optional<sdk::error> postpublish_error;
		bridge.publish_once =
			[&writer,
			 &published,
			 &publish_error,
			 &postpublish_error,
			 expected_for_postpublish,
			 &output_authority,
			 &result_digest,
			 v6_sqlite_path](std::string_view,
							 std::string_view) -> bounded_store_publication_terminal
		{
			auto result = writer->publish();
			if (!result)
			{
				publish_error = result.error();
				return result.error().code == "store.publication-conflict"
					? bounded_store_publication_terminal::rejected_stale
					: bounded_store_publication_terminal::publication_outcome_unknown;
			}
			published.emplace(std::move(*result));
			auto actual =
				sdk::snapshot_store_backend_lifetime_access::published_projection(*published);
			if (!actual)
			{
				postpublish_error = sdk::error{"materialization.writer-postpublish-mismatch",
											   "published-projection",
											   actual.error().code + ":" + actual.error().field +
												   ":" + actual.error().detail};
				return bounded_store_publication_terminal::committed_unverified;
			}
			std::vector<bounded_store_record> published_records;
			published_records.reserve(actual->records.size());
			for (auto& record : actual->records)
				published_records.emplace_back(bounded_store_record{projection_kind(record.kind),
																	std::move(record.key),
																	std::move(record.payload)});
			if (auto verified = run_v6_candidate_preflight(
					output_authority.snapshot.series,
					output_authority.snapshot.series.engine_generation_id,
					std::span<const bounded_store_record>{expected_for_postpublish},
					std::span<const bounded_store_record>{published_records},
					result_digest,
					v6_sqlite_path ? std::optional<std::string_view>{*v6_sqlite_path}
								   : std::nullopt);
				!verified)
			{
				postpublish_error =
					sdk::error{"materialization.writer-postpublish-mismatch",
							   "physical-projection",
							   verified.error().code + ":" + verified.error().field + ":" +
								   verified.error().detail};
				return bounded_store_publication_terminal::committed_unverified;
			}
			return bounded_store_publication_terminal::committed_verified;
		};
		auto bridged = run_materialization_store_candidate_bridge(std::move(bridge));
		if (publish_error)
			return sdk::unexpected(std::move(*publish_error));
		if (postpublish_error)
			return sdk::unexpected(std::move(*postpublish_error));
		if (!bridged)
			return sdk::unexpected(std::move(bridged.error()));
		if (!published)
			return sdk::unexpected(
				sdk::error{"materialization.store-publication-missing", "publish", "missing"});
		return materialization_store_publication{std::move(*published),
												 output_authority,
												 materialization_request_id,
												 task_id,
												 task_input_digest,
												 result_digest,
												 source_receipt_digest,
												 source_terminal,
												 true};
	}
} // namespace cxxlens::sdk::detail
