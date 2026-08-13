#include "materialization_prior_artifact.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "materialization_json.hpp"
#include "materialization_rooted_vfs.hpp"
#include "sdk/provider_runtime_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::string_view artifact_schema{"cxxlens.clang22.incremental-artifact.v1"};
		constexpr std::uint64_t artifact_version = 1U;
		constexpr std::size_t max_memory_artifacts = 64U;
		constexpr std::size_t max_memory_artifact_bytes =
			detailed_report_limits::maximum_report_bytes;
		std::atomic<std::uint64_t> sidecar_attempt_counter{};

		struct memory_prior_artifact_store
		{
			std::mutex mutex;
			std::map<std::string, std::vector<std::byte>, std::less<>> artifacts;
			std::size_t resident_bytes{};
		};

		[[nodiscard]] memory_prior_artifact_store& memory_store()
		{
			static memory_prior_artifact_store value;
			return value;
		}

		[[nodiscard]] sdk::error artifact_error(std::string field, std::string detail = {})
		{
			return {"materialization.incremental-artifact-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] bool
		valid_artifact_limits(const materialization_prior_artifact_limits& limits) noexcept
		{
			return limits.max_bytes != 0U && limits.max_tasks != 0U &&
				limits.max_capture_bytes != 0U && limits.max_total_capture_bytes != 0U &&
				limits.max_batches_per_task != 0U && limits.max_chunks_per_batch != 0U &&
				limits.max_side_channel_records != 0U && limits.max_string_bytes != 0U &&
				limits.max_capture_bytes <= limits.max_bytes &&
				limits.max_total_capture_bytes <= limits.max_bytes;
		}

		[[nodiscard]] detailed_report_limits
		capture_limits(const materialization_prior_artifact_limits& limits) noexcept
		{
			detailed_report_limits output;
			output.max_tasks = 1U;
			output.max_batches_per_task = limits.max_batches_per_task;
			output.max_chunks_per_batch = limits.max_chunks_per_batch;
			output.max_side_channel_records = limits.max_side_channel_records;
			output.max_string_bytes = limits.max_string_bytes;
			output.max_projection_bytes =
				std::min({limits.max_bytes, limits.max_capture_bytes, output.maximum_report_bytes});
			return output;
		}

		[[nodiscard]] bool canonical_content_digest(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool canonical_semantic_digest(const std::string_view value) noexcept
		{
			return value.size() == 83U && value.starts_with("semantic-v2:sha256:") &&
				std::ranges::all_of(value.substr(19U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool canonical_artifact_digest(const std::string_view value) noexcept
		{
			constexpr std::string_view prefix{
				"materialization.incremental-sealed-artifact:sha256:"};
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				std::ranges::all_of(value.substr(prefix.size()),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] sdk::canonical_value string_value(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::result<sdk::canonical_value> integer_value(const std::uint64_t value)
		{
			if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				return sdk::unexpected(artifact_error("integer", "signed-range"));
			return sdk::canonical_value::from_integer(static_cast<std::int64_t>(value));
		}

		[[nodiscard]] sdk::result<std::uint64_t> integer(const sdk::canonical_value& value,
														 const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::signed_integer || value.integer < 0)
				return sdk::unexpected(artifact_error(std::string{field}, "integer"));
			return static_cast<std::uint64_t>(value.integer);
		}

		[[nodiscard]] sdk::result<std::string> string(const sdk::canonical_value& value,
													  const std::string_view field,
													  const bool require_nonempty = true)
		{
			if (value.type != sdk::canonical_value::kind::utf8_string ||
				(require_nonempty && value.text.empty()) || value.text.contains('\0'))
				return sdk::unexpected(artifact_error(std::string{field}, "string"));
			return value.text;
		}

		[[nodiscard]] sdk::result<std::span<const sdk::canonical_value>>
		tuple(const sdk::canonical_value& value,
			  const std::size_t expected,
			  const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple ||
				value.tuple.size() != expected)
				return sdk::unexpected(artifact_error(std::string{field}, "tuple-shape"));
			return std::span<const sdk::canonical_value>{value.tuple};
		}

		[[nodiscard]] sdk::result<std::span<const sdk::canonical_value>>
		bounded_tuple(const sdk::canonical_value& value,
					  const std::size_t maximum,
					  const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple || value.tuple.empty() ||
				value.tuple.size() > maximum)
				return sdk::unexpected(artifact_error(std::string{field}, "bounded-tuple-shape"));
			return std::span<const sdk::canonical_value>{value.tuple};
		}

		[[nodiscard]] sdk::result<std::optional<std::string>>
		optional_string(const sdk::canonical_value& value, const std::string_view field)
		{
			if (value.type == sdk::canonical_value::kind::null_value)
				return std::optional<std::string>{};
			auto decoded = string(value, field);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			return std::optional<std::string>{std::move(*decoded)};
		}

		[[nodiscard]] sdk::result<bool> boolean(const sdk::canonical_value& value,
												const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::boolean)
				return sdk::unexpected(artifact_error(std::string{field}, "boolean"));
			return value.boolean;
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>> bytes(const sdk::canonical_value& value,
																const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::bytes)
				return sdk::unexpected(artifact_error(std::string{field}, "bytes"));
			return value.byte_string;
		}

		[[nodiscard]] sdk::canonical_value
		input_value(const sdk::incremental::input_fingerprint& input)
		{
			return sdk::canonical_value::from_tuple({
				string_value(input.source_digest),
				string_value(input.dependency_digest),
				string_value(input.invocation_digest),
				string_value(input.toolchain_digest),
				string_value(input.condition_universe_digest),
				string_value(input.variant_digest),
				string_value(input.provider_set_digest),
				string_value(input.registry_digest),
				string_value(input.interpretation_policy_digest),
				string_value(input.refresh_policy_digest),
				string_value(input.environment_digest),
				string_value(input.provider_binary_digest),
				string_value(input.provider_semantics_digest),
				string_value(input.relation_descriptor_digest),
				string_value(input.normalizer_version),
				string_value(input.model_digest),
				string_value(input.assumption_digest),
				string_value(input.precision_profile),
			});
		}

		[[nodiscard]] sdk::result<sdk::incremental::input_fingerprint>
		decode_input(const sdk::canonical_value& value)
		{
			auto fields = tuple(value, 18U, "state.input");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			sdk::incremental::input_fingerprint output;
			std::array<std::string*, 18U> destinations{
				&output.source_digest,
				&output.dependency_digest,
				&output.invocation_digest,
				&output.toolchain_digest,
				&output.condition_universe_digest,
				&output.variant_digest,
				&output.provider_set_digest,
				&output.registry_digest,
				&output.interpretation_policy_digest,
				&output.refresh_policy_digest,
				&output.environment_digest,
				&output.provider_binary_digest,
				&output.provider_semantics_digest,
				&output.relation_descriptor_digest,
				&output.normalizer_version,
				&output.model_digest,
				&output.assumption_digest,
				&output.precision_profile,
			};
			for (std::size_t index{}; index < destinations.size(); ++index)
			{
				auto field = string((*fields)[index], "state.input.field");
				if (!field)
					return sdk::unexpected(std::move(field.error()));
				*destinations[index] = std::move(*field);
			}
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(artifact_error("state.input", "validation"));
			return output;
		}

		[[nodiscard]] sdk::canonical_value
		state_value(const sdk::incremental::partition_state& state)
		{
			return sdk::canonical_value::from_tuple({
				string_value(state.partition_id),
				input_value(state.input),
				string_value(state.coverage_digest),
				string_value(state.closure_digest),
				sdk::canonical_value::from_boolean(state.corruption_detected),
			});
		}

		[[nodiscard]] sdk::result<sdk::incremental::partition_state>
		decode_state(const sdk::canonical_value& value)
		{
			auto fields = tuple(value, 5U, "state");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto partition_id = string((*fields)[0U], "state.partition_id");
			auto input = decode_input((*fields)[1U]);
			auto coverage = string((*fields)[2U], "state.coverage_digest");
			auto closure = string((*fields)[3U], "state.closure_digest");
			auto corruption = boolean((*fields)[4U], "state.corruption_detected");
			if (!partition_id || !input || !coverage || !closure || !corruption)
				return sdk::unexpected(artifact_error("state", "field"));
			sdk::incremental::partition_state output{std::move(*partition_id),
													 std::move(*input),
													 std::move(*coverage),
													 std::move(*closure),
													 *corruption};
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(artifact_error("state", "validation"));
			return output;
		}

		[[nodiscard]] sdk::canonical_value
		selector_value(const sdk::snapshot_series_selector& selector)
		{
			return sdk::canonical_value::from_tuple({
				string_value(selector.catalog_id),
				string_value(selector.channel_id),
				string_value(selector.engine_generation_id),
				string_value(selector.condition_universe_id),
				string_value(selector.relation_registry_digest),
				string_value(selector.interpretation_policy_digest),
				string_value(selector.trust_policy_digest),
			});
		}

		[[nodiscard]] sdk::result<sdk::snapshot_series_selector>
		decode_selector(const sdk::canonical_value& value)
		{
			auto fields = tuple(value, 7U, "selector");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			sdk::snapshot_series_selector output;
			std::array<std::string*, 7U> destinations{
				&output.catalog_id,
				&output.channel_id,
				&output.engine_generation_id,
				&output.condition_universe_id,
				&output.relation_registry_digest,
				&output.interpretation_policy_digest,
				&output.trust_policy_digest,
			};
			for (std::size_t index{}; index < destinations.size(); ++index)
			{
				auto field = string((*fields)[index], "selector.field");
				if (!field)
					return sdk::unexpected(std::move(field.error()));
				*destinations[index] = std::move(*field);
			}
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(artifact_error("selector", "validation"));
			return output;
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		make_publication_value(const materialization_prior_artifact_bundle& bundle)
		{
			auto sequence = integer_value(bundle.sequence);
			if (!sequence)
				return sdk::unexpected(std::move(sequence.error()));
			auto generation = integer_value(bundle.physical_generation);
			if (!generation)
				return sdk::unexpected(std::move(generation.error()));
			if (!sdk::is_valid(bundle.publication_state))
				return sdk::unexpected(artifact_error("publication.state", "closed-enum"));
			return sdk::canonical_value::from_tuple({
				string_value(bundle.publication_id),
				string_value(bundle.series_id),
				string_value(bundle.snapshot_id),
				*sequence,
				*generation,
				bundle.parent_publication ? string_value(*bundle.parent_publication)
										  : sdk::canonical_value::null(),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(bundle.publication_state)),
				sdk::canonical_value::from_boolean(bundle.publication_corrupt),
			});
		}

		[[nodiscard]] sdk::result<std::tuple<std::string,
											 std::string,
											 std::string,
											 std::uint64_t,
											 std::uint64_t,
											 std::optional<std::string>,
											 sdk::publication_state,
											 bool>>
		decode_publication(const sdk::canonical_value& value)
		{
			auto fields = tuple(value, 8U, "publication");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto publication_id = string((*fields)[0U], "publication.id");
			auto series_id = string((*fields)[1U], "publication.series");
			auto snapshot_id = string((*fields)[2U], "publication.snapshot");
			auto sequence = integer((*fields)[3U], "publication.sequence");
			auto generation = integer((*fields)[4U], "publication.generation");
			auto parent = optional_string((*fields)[5U], "publication.parent");
			auto state = integer((*fields)[6U], "publication.state");
			auto corrupt = boolean((*fields)[7U], "publication.corrupt");
			if (!publication_id || !series_id || !snapshot_id || !sequence || !generation ||
				!parent || !state || !corrupt ||
				*state > static_cast<std::uint64_t>(sdk::publication_state::rolled_back))
				return sdk::unexpected(artifact_error("publication", "field"));
			return std::tuple{std::move(*publication_id),
							  std::move(*series_id),
							  std::move(*snapshot_id),
							  *sequence,
							  *generation,
							  std::move(*parent),
							  static_cast<sdk::publication_state>(*state),
							  *corrupt};
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		task_value(const materialization_prior_artifact_task& task,
				   const materialization_prior_artifact_limits& limits)
		{
			auto capture =
				encode_detailed_task_report_capture(task.capture, capture_limits(limits));
			if (!capture || capture->size() > limits.max_capture_bytes)
				return sdk::unexpected(artifact_error("task.capture", "encode-or-limit"));
			auto ordinal = integer_value(task.identity.canonical_task_ordinal);
			if (!ordinal)
				return sdk::unexpected(std::move(ordinal.error()));
			return sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_tuple({
					*ordinal,
					string_value(task.identity.provider_task_id),
					string_value(task.identity.task_input_digest),
					string_value(task.identity.selected_catalog_compile_unit_id),
					string_value(task.identity.final_relation_compile_unit_id),
				}),
				state_value(task.state),
				string_value(task.sealed_artifact_digest),
				sdk::canonical_value::from_bytes(std::move(*capture)),
			});
		}

		[[nodiscard]] sdk::result<materialization_prior_artifact_task>
		decode_task(const sdk::canonical_value& value,
					const materialization_prior_artifact_limits& limits)
		{
			auto fields = tuple(value, 4U, "task");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto identity = tuple((*fields)[0U], 5U, "task.identity");
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			auto ordinal = integer((*identity)[0U], "task.ordinal");
			auto provider_task = string((*identity)[1U], "task.provider_task_id");
			auto input_digest = string((*identity)[2U], "task.task_input_digest");
			auto selected = string((*identity)[3U], "task.selected_compile_unit");
			auto final = string((*identity)[4U], "task.final_compile_unit");
			auto state = decode_state((*fields)[1U]);
			auto artifact_digest = string((*fields)[2U], "task.artifact_digest");
			auto capture_bytes = bytes((*fields)[3U], "task.capture");
			if (!ordinal || !provider_task || !input_digest || !selected || !final || !state ||
				!artifact_digest || !capture_bytes ||
				*ordinal > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
				capture_bytes->size() > limits.max_capture_bytes ||
				!canonical_artifact_digest(*artifact_digest))
				return sdk::unexpected(artifact_error("task", "field"));
			auto capture =
				decode_detailed_task_report_capture(*capture_bytes, capture_limits(limits));
			if (!capture || capture->provider_task_id != *provider_task ||
				capture->task_input_digest != *input_digest ||
				capture->selected_catalog_compile_unit_id != *selected ||
				capture->compile_unit_id != *final)
				return sdk::unexpected(artifact_error("task.capture", "identity"));
			return materialization_prior_artifact_task{
				materialization_incremental_task_identity{static_cast<std::size_t>(*ordinal),
														  std::move(*provider_task),
														  std::move(*input_digest),
														  std::move(*selected),
														  std::move(*final)},
				std::move(*state),
				std::move(*artifact_digest),
				std::move(*capture)};
		}

		[[nodiscard]] sdk::result<void>
		validate_bundle(const materialization_prior_artifact_bundle& bundle,
						const materialization_prior_artifact_limits& limits)
		{
			if (bundle.schema != artifact_schema || bundle.version != artifact_version ||
				bundle.series_id.empty() || bundle.publication_id.empty() ||
				bundle.snapshot_id.empty() || bundle.series_id.contains('\0') ||
				bundle.publication_id.contains('\0') || bundle.snapshot_id.contains('\0') ||
				bundle.publication_state != sdk::publication_state::committed ||
				bundle.publication_corrupt || bundle.tasks.empty() ||
				bundle.tasks.size() > limits.max_tasks)
				return sdk::unexpected(artifact_error("bundle", "shape"));
			if (!sdk::validate_strong_id(bundle.series_id) ||
				!sdk::validate_strong_id(bundle.publication_id) ||
				!sdk::validate_strong_id(bundle.snapshot_id) ||
				(bundle.parent_publication && !sdk::validate_strong_id(*bundle.parent_publication)))
				return sdk::unexpected(artifact_error("bundle.publication", "strong-id"));
			if (auto valid = bundle.selector.validate();
				!valid || bundle.series_id != bundle.selector.id())
				return sdk::unexpected(artifact_error("bundle.selector", "binding"));
			std::set<std::string, std::less<>> partitions;
			std::size_t previous{};
			bool first{true};
			for (const auto& task : bundle.tasks)
			{
				if ((first && task.identity.canonical_task_ordinal != 0U) ||
					(!first &&
					 (task.identity.canonical_task_ordinal <= previous ||
					  task.identity.canonical_task_ordinal != previous + 1U)) ||
					!partitions.insert(task.state.partition_id).second ||
					task.identity.provider_task_id != task.capture.provider_task_id ||
					task.identity.task_input_digest != task.capture.task_input_digest ||
					task.identity.selected_catalog_compile_unit_id !=
						task.capture.selected_catalog_compile_unit_id ||
					task.identity.final_relation_compile_unit_id != task.capture.compile_unit_id ||
					!sdk::validate_strong_id(task.identity.provider_task_id) ||
					!sdk::validate_strong_id(task.identity.task_input_digest) ||
					!sdk::validate_strong_id(task.identity.selected_catalog_compile_unit_id) ||
					!sdk::validate_strong_id(task.identity.final_relation_compile_unit_id) ||
					!canonical_artifact_digest(task.sealed_artifact_digest))
					return sdk::unexpected(artifact_error("bundle.tasks", "identity-or-order"));
				if (auto valid = task.state.validate(); !valid)
					return sdk::unexpected(artifact_error("bundle.tasks.state", "validation"));
				previous = task.identity.canonical_task_ordinal;
				first = false;
			}
			return {};
		}

		[[nodiscard]] sdk::result<std::string> sidecar_path(const std::string_view sqlite_path,
															const std::string_view publication_id)
		{
			if (auto valid = validate_materialization_sqlite_path(sqlite_path); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (!sdk::validate_strong_id(publication_id))
				return sdk::unexpected(artifact_error("sidecar.publication", "strong-id"));
			const auto id_bytes =
				std::as_bytes(std::span<const char>{publication_id.data(), publication_id.size()});
			const auto locator_digest = sdk::content_digest(id_bytes);
			if (!canonical_content_digest(locator_digest))
				return sdk::unexpected(artifact_error("sidecar.publication", "digest"));
			std::string path{sqlite_path};
			constexpr std::string_view suffix{".cxxlens-incremental-v1-"};
			if (path.size() > std::numeric_limits<std::size_t>::max() - suffix.size() - 64U)
				return sdk::unexpected(artifact_error("sidecar.path", "overflow"));
			path += suffix;
			path.append(locator_digest.substr(7U));
			if (auto valid = validate_materialization_relative_path(path, 4095U, true); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return path;
		}

		[[nodiscard]] bool is_missing_sidecar(const sdk::error& error) noexcept
		{
			return error.code == "materialization.identity-mismatch" && error.field == "openat2" &&
				error.detail == std::to_string(ENOENT);
		}

		[[nodiscard]] bool is_errno_error(const sdk::error& error,
										  const std::string_view field,
										  const int value) noexcept
		{
			return error.code == "materialization.identity-mismatch" && error.field == field &&
				error.detail == std::to_string(value);
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		read_sidecar(const materialization_owned_fd& file,
					 const materialization_prior_artifact_limits& limits)
		{
			auto identity = materialization_fd_identity(file.get(), true);
			if (!identity || identity->size_bytes == 0U ||
				identity->size_bytes > limits.max_bytes ||
				identity->size_bytes > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(artifact_error("sidecar", "size"));
			std::vector<std::byte> bytes(static_cast<std::size_t>(identity->size_bytes));
			std::size_t offset{};
			while (offset < bytes.size())
			{
				const auto count = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0 || static_cast<std::size_t>(count) > bytes.size() - offset)
					return sdk::unexpected(artifact_error("sidecar", "read"));
				offset += static_cast<std::size_t>(count);
			}
			return bytes;
		}

		[[nodiscard]] sdk::result<void> write_sidecar(const materialization_owned_fd& file,
													  const std::span<const std::byte> bytes)
		{
			std::size_t offset{};
			while (offset < bytes.size())
			{
				const auto count =
					::write(file.get(), bytes.data() + offset, bytes.size() - offset);
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0 || static_cast<std::size_t>(count) > bytes.size() - offset)
					return sdk::unexpected(artifact_error("sidecar", "write"));
				offset += static_cast<std::size_t>(count);
			}
			if (::fsync(file.get()) != 0)
				return sdk::unexpected(artifact_error("sidecar", "sync"));
			return {};
		}

		[[nodiscard]] sdk::result<void> sync_sidecar_parent(const materialization_effect_root& root,
															const std::string_view path);

		[[nodiscard]] sdk::result<void>
		install_sidecar(const materialization_effect_root& root,
						const std::string_view path,
						const std::span<const std::byte> bytes,
						const materialization_prior_artifact_limits& limits)
		{
			auto existing = root.open_beneath(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (existing)
			{
				auto current = read_sidecar(*existing, limits);
				if (!current)
					return sdk::unexpected(std::move(current.error()));
				if (*current != std::vector<std::byte>{bytes.begin(), bytes.end()})
					return sdk::unexpected(artifact_error("sidecar", "immutable-conflict"));
				return {};
			}
			if (!is_missing_sidecar(existing.error()))
				return sdk::unexpected(std::move(existing.error()));

			const auto payload_digest = sdk::content_digest(bytes);
			if (payload_digest.rfind("sha256:", 0U) != 0U || payload_digest.size() <= 7U)
				return sdk::unexpected(artifact_error("sidecar", "payload-digest"));
			const auto attempt = sidecar_attempt_counter.fetch_add(1U, std::memory_order_relaxed);
			std::string temporary_path{path};
			std::string temporary_suffix{".tmp-"};
			temporary_suffix.append(payload_digest.substr(7U));
			temporary_suffix.push_back('-');
			temporary_suffix.append(std::to_string(static_cast<unsigned long long>(::getpid())));
			temporary_suffix.push_back('-');
			temporary_suffix.append(std::to_string(static_cast<unsigned long long>(attempt)));
			if (temporary_path.size() >
				std::numeric_limits<std::size_t>::max() - temporary_suffix.size())
				return sdk::unexpected(artifact_error("sidecar", "temporary-path-overflow"));
			temporary_path += temporary_suffix;
			if (auto valid = validate_materialization_relative_path(temporary_path, 4095U, true);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto remove_temporary = [&]() -> sdk::result<void>
			{
				auto removed = root.unlink_beneath(temporary_path);
				if (!removed && !is_errno_error(removed.error(), "unlinkat", ENOENT))
					return removed;
				return {};
			};

			auto temporary = root.open_beneath(
				temporary_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600U);
			if (temporary)
			{
				if (auto written = write_sidecar(*temporary, bytes); !written)
				{
					if (auto removed = remove_temporary(); !removed)
						return removed;
					return written;
				}
			}
			else if (is_errno_error(temporary.error(), "openat2", EEXIST))
				return sdk::unexpected(artifact_error("sidecar", "temporary-conflict"));
			else
				return sdk::unexpected(std::move(temporary.error()));

			auto renamed = root.rename_beneath(temporary_path, path);
			if (!renamed)
			{
				if (is_errno_error(renamed.error(), "renameat2", EEXIST) ||
					is_errno_error(renamed.error(), "renameat2", ENOENT))
				{
					auto raced = root.open_beneath(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
					if (!raced)
					{
						if (auto removed = remove_temporary(); !removed)
							return removed;
						return sdk::unexpected(std::move(raced.error()));
					}
					auto raced_bytes = read_sidecar(*raced, limits);
					if (!raced_bytes ||
						*raced_bytes != std::vector<std::byte>{bytes.begin(), bytes.end()})
					{
						if (auto removed = remove_temporary(); !removed)
							return removed;
						return sdk::unexpected(artifact_error("sidecar", "immutable-conflict"));
					}
					if (auto removed = remove_temporary(); !removed)
						return removed;
					return {};
				}
				if (auto removed = remove_temporary(); !removed)
					return removed;
				return renamed;
			}
			else if (auto synced = sync_sidecar_parent(root, path); !synced)
				return synced;
			return {};
		}

		[[nodiscard]] sdk::result<void> sync_sidecar_parent(const materialization_effect_root& root,
															const std::string_view path)
		{
			const auto separator = path.rfind('/');
			materialization_owned_fd parent;
			if (separator == std::string_view::npos)
			{
				auto duplicated = root.duplicate_directory();
				if (!duplicated)
					return sdk::unexpected(artifact_error("sidecar.parent", "duplicate"));
				parent = std::move(*duplicated);
			}
			else
			{
				auto opened = root.open_beneath(path.substr(0U, separator),
												O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
				if (!opened)
					return sdk::unexpected(artifact_error("sidecar.parent", "open"));
				parent = std::move(*opened);
			}
			if (!parent || ::fsync(parent.get()) != 0)
				return sdk::unexpected(artifact_error("sidecar.parent", "sync"));
			return {};
		}

		[[nodiscard]] sdk::result<sdk::detached_row> decode_row(std::string raw)
		{
			json_limits limits;
			limits.max_input_bytes = std::max<std::size_t>(raw.size(), 1U);
			limits.max_string_bytes = std::max<std::size_t>(raw.size(), 1U);
			limits.max_total_string_bytes = std::max<std::size_t>(raw.size() * 2U, 1U);
			auto document = parse_json_object(std::move(raw), limits);
			if (!document)
				return sdk::unexpected(artifact_error("row", "json"));
			const auto& root = document->root();
			constexpr std::array members{std::string_view{"cells"},
										 std::string_view{"descriptor_id"}};
			if (!root.has_exact_members(members))
				return sdk::unexpected(artifact_error("row", "members"));
			auto descriptor_value = root.member("descriptor_id");
			auto cells_value = root.member("cells");
			if (descriptor_value == nullptr || cells_value == nullptr ||
				descriptor_value->as_string() == nullptr || cells_value->as_object() == nullptr)
				return sdk::unexpected(artifact_error("row", "shape"));
			sdk::detached_row row{*descriptor_value->as_string(), {}};
			for (const auto& [column_id, value] : *cells_value->as_object())
			{
				const auto* cell_object = value.as_object();
				if (cell_object == nullptr)
					return sdk::unexpected(artifact_error("row.cell", "object"));
				for (const auto& [member_name, member_value] : *cell_object)
				{
					(void)member_value;
					if (member_name != "state" && member_name != "type" && member_name != "value" &&
						member_name != "unknown_reason")
						return sdk::unexpected(artifact_error("row.cell", "unknown-member"));
				}
				const auto* state_value = value.member("state");
				const auto* type_value = value.member("type");
				if (state_value == nullptr || type_value == nullptr ||
					state_value->as_string() == nullptr || type_value->as_string() == nullptr)
					return sdk::unexpected(artifact_error("row.cell", "shape"));
				const auto& state_name = *state_value->as_string();
				sdk::cell_state state;
				if (state_name == "present")
					state = sdk::cell_state::present;
				else if (state_name == "absent")
					state = sdk::cell_state::absent;
				else if (state_name == "unknown")
					state = sdk::cell_state::unknown;
				else
					return sdk::unexpected(artifact_error("row.cell.state", "enum"));
				const auto& type_name = *type_value->as_string();
				bool optional{};
				std::string base{type_name};
				if (base.starts_with("optional<") && base.ends_with('>'))
				{
					optional = true;
					base = base.substr(9U, base.size() - 10U);
				}
				std::string parameter;
				const auto open = base.find('<');
				if (open != std::string::npos)
				{
					if (base.back() != '>' || open == 0U || open + 2U > base.size())
						return sdk::unexpected(artifact_error("row.cell.type", "parameter"));
					parameter = base.substr(open + 1U, base.size() - open - 2U);
					base.resize(open);
				}
				static const std::map<std::string_view, sdk::scalar_kind, std::less<>> kinds{
					{"bool", sdk::scalar_kind::boolean},
					{"int64", sdk::scalar_kind::signed_integer},
					{"uint64", sdk::scalar_kind::unsigned_integer},
					{"utf8_string", sdk::scalar_kind::utf8_string},
					{"bytes", sdk::scalar_kind::bytes},
					{"digest", sdk::scalar_kind::digest},
					{"semantic_version", sdk::scalar_kind::semantic_version},
					{"typed_id", sdk::scalar_kind::typed_id},
					{"open_symbol", sdk::scalar_kind::open_symbol},
					{"condition_ref", sdk::scalar_kind::condition_ref},
					{"source_span_id", sdk::scalar_kind::source_span_id},
					{"evidence_id", sdk::scalar_kind::evidence_id},
					{"closed_symbol", sdk::scalar_kind::closed_symbol},
					{"set", sdk::scalar_kind::set},
					{"relation_name", sdk::scalar_kind::relation_name},
					{"semantic_key_id", sdk::scalar_kind::semantic_key_id},
					{"assertion_id", sdk::scalar_kind::assertion_id},
					{"content_digest", sdk::scalar_kind::content_digest},
					{"interpretation_domain_id", sdk::scalar_kind::interpretation_domain_id},
				};
				auto kind = kinds.find(base);
				if (kind == kinds.end())
					return sdk::unexpected(artifact_error("row.cell.type", "scalar"));
				sdk::value_type cell_type{kind->second, std::move(parameter), optional};
				if (cell_type.canonical_name() != type_name)
					return sdk::unexpected(artifact_error("row.cell.type", "canonical"));
				sdk::detached_cell cell{cell_type, state, std::nullopt, std::nullopt};
				const auto* value_member = value.member("value");
				const auto* reason_member = value.member("unknown_reason");
				if (state == sdk::cell_state::present)
				{
					if (value_member == nullptr || reason_member != nullptr)
						return sdk::unexpected(artifact_error("row.cell", "present-shape"));
					switch (cell_type.scalar)
					{
						case sdk::scalar_kind::boolean:
							if (value_member->as_boolean() == nullptr)
								return sdk::unexpected(artifact_error("row.cell.value", "boolean"));
							cell.value = sdk::scalar_value{*value_member->as_boolean()};
							break;
						case sdk::scalar_kind::signed_integer:
						{
							if (value_member->as_signed_integer() != nullptr)
								cell.value = sdk::scalar_value{*value_member->as_signed_integer()};
							else if (value_member->as_unsigned_integer() != nullptr &&
									 *value_member->as_unsigned_integer() <=
										 static_cast<std::uint64_t>(
											 std::numeric_limits<std::int64_t>::max()))
								cell.value = sdk::scalar_value{static_cast<std::int64_t>(
									*value_member->as_unsigned_integer())};
							else
								return sdk::unexpected(artifact_error("row.cell.value", "int64"));
							break;
						}
						case sdk::scalar_kind::unsigned_integer:
							if (value_member->as_unsigned_integer() != nullptr)
								cell.value =
									sdk::scalar_value{*value_member->as_unsigned_integer()};
							else if (value_member->as_signed_integer() != nullptr &&
									 *value_member->as_signed_integer() >= 0)
								cell.value = sdk::scalar_value{
									static_cast<std::uint64_t>(*value_member->as_signed_integer())};
							else
								return sdk::unexpected(artifact_error("row.cell.value", "uint64"));
							break;
						case sdk::scalar_kind::bytes:
						case sdk::scalar_kind::set:
						{
							if (value_member->as_string() == nullptr ||
								value_member->as_string()->size() % 2U != 0U)
								return sdk::unexpected(artifact_error("row.cell.value", "hex"));
							std::vector<std::byte> decoded;
							decoded.reserve(value_member->as_string()->size() / 2U);
							for (std::size_t index{}; index < value_member->as_string()->size();
								 index += 2U)
							{
								const auto hex = value_member->as_string()->substr(index, 2U);
								unsigned value_byte{};
								auto result =
									std::from_chars(hex.data(), hex.data() + 2U, value_byte, 16);
								if (result.ec != std::errc{} || result.ptr != hex.data() + 2U)
									return sdk::unexpected(artifact_error("row.cell.value", "hex"));
								decoded.push_back(static_cast<std::byte>(value_byte));
							}
							cell.value = sdk::scalar_value{std::move(decoded)};
							break;
						}
						default:
							if (value_member->as_string() == nullptr)
								return sdk::unexpected(artifact_error("row.cell.value", "string"));
							cell.value = sdk::scalar_value{*value_member->as_string()};
							break;
					}
				}
				else if (state == sdk::cell_state::absent)
				{
					if (value_member != nullptr || reason_member != nullptr)
						return sdk::unexpected(artifact_error("row.cell", "absent-shape"));
				}
				else
				{
					if (value_member != nullptr || reason_member == nullptr ||
						reason_member->as_string() == nullptr)
						return sdk::unexpected(artifact_error("row.cell", "unknown-shape"));
					cell.unknown_reason = *reason_member->as_string();
				}
				if (auto valid = cell.validate(); !valid)
					return sdk::unexpected(artifact_error("row.cell", "validation"));
				if (!row.cells.emplace(column_id, std::move(cell)).second)
					return sdk::unexpected(artifact_error("row.cell", "duplicate"));
			}
			if (row.canonical_form() != document->raw_bytes())
				return sdk::unexpected(artifact_error("row", "noncanonical"));
			return row;
		}

		[[nodiscard]] sdk::result<std::string>
		recompute_row_set_digest(const detailed_provider_batch_projection& batch)
		{
			std::string projection;
			for (std::size_t index{}; index < batch.rows.size(); ++index)
			{
				if (batch.rows[index].row_index != index)
					return sdk::unexpected(artifact_error("task.capture.rows", "order"));
				if (!canonical_content_digest(batch.rows[index].row_digest) ||
					sdk::content_digest(
						std::as_bytes(std::span{batch.rows[index].row_canonical_form.data(),
												batch.rows[index].row_canonical_form.size()})) !=
						batch.rows[index].row_digest)
					return sdk::unexpected(artifact_error("task.capture.rows", "digest"));
				projection += std::to_string(index);
				projection.push_back(':');
				projection += batch.rows[index].row_canonical_form;
				projection.push_back('\n');
			}
			return sdk::semantic_digest("cxxlens.clang22.materialization-report.row-set.v1",
										projection);
		}

		[[nodiscard]] sdk::result<void>
		validate_capture_against_result(const detailed_task_report_capture& capture,
										const sealed_materialization_result& result)
		{
			const auto transcript = result.provider_seal();
			if (capture.batches.size() != transcript.batches().size() ||
				capture.coverage.size() != transcript.coverage().size() ||
				capture.unresolved.size() != transcript.unresolved().size() ||
				capture.evidence.size() != transcript.evidence().size() ||
				capture.base_claim_rows.size() != result.base_claim_rows().size() ||
				capture.source_span_claim_rows.size() != result.source_span_claim_rows().size())
				return sdk::unexpected(artifact_error("task.capture", "result-shape"));
			for (std::size_t index{}; index < capture.batches.size(); ++index)
			{
				const auto& left = capture.batches[index];
				const auto& right = transcript.batches()[index];
				if (left.task_id != right.task_id() ||
					left.descriptor_id != right.descriptor_id() ||
					left.descriptor_digest != right.descriptor_digest() ||
					left.dependency_group_id != right.dependency_group_id() ||
					left.atomic_output_group_id != right.atomic_output_group_id() ||
					left.batch_id != right.batch_id() ||
					left.batch_digest != right.batch_digest() ||
					left.columns.size() != right.columns().size() ||
					left.ordered_chunk_digests.size() != right.ordered_chunk_digests().size() ||
					left.rows.size() != right.rows().size())
					return sdk::unexpected(artifact_error("task.capture.batches", "binding"));
				if (!std::ranges::equal(left.ordered_chunk_digests, right.ordered_chunk_digests()))
					return sdk::unexpected(artifact_error("task.capture.batches", "chunks"));
				if (!std::ranges::equal(left.columns, right.columns()))
					return sdk::unexpected(artifact_error("task.capture.batches", "columns"));
				for (std::size_t row_index{}; row_index < left.rows.size(); ++row_index)
					if (left.rows[row_index].row_canonical_form !=
						right.rows()[row_index].canonical_form())
						return sdk::unexpected(artifact_error("task.capture.batches", "rows"));
				auto row_set = recompute_row_set_digest(left);
				if (!row_set || left.row_set_digest != *row_set)
					return sdk::unexpected(
						artifact_error("task.capture.batches", "row-set-digest"));
			}
			for (std::size_t index{}; index < capture.coverage.size(); ++index)
				if (capture.coverage[index].kind != transcript.coverage()[index].kind ||
					capture.coverage[index].id != transcript.coverage()[index].id ||
					capture.coverage[index].state != transcript.coverage()[index].state ||
					capture.coverage[index].reason != transcript.coverage()[index].reason)
					return sdk::unexpected(artifact_error("task.capture.coverage", "binding"));
			for (std::size_t index{}; index < capture.unresolved.size(); ++index)
				if (capture.unresolved[index].code != transcript.unresolved()[index].code ||
					capture.unresolved[index].subject != transcript.unresolved()[index].subject ||
					capture.unresolved[index].detail != transcript.unresolved()[index].detail)
					return sdk::unexpected(artifact_error("task.capture.unresolved", "binding"));
			for (std::size_t index{}; index < capture.evidence.size(); ++index)
				if (capture.evidence[index].kind != transcript.evidence()[index].kind ||
					capture.evidence[index].subject != transcript.evidence()[index].subject ||
					capture.evidence[index].producer != transcript.evidence()[index].producer ||
					capture.evidence[index].summary != transcript.evidence()[index].summary)
					return sdk::unexpected(artifact_error("task.capture.evidence", "binding"));
			for (std::size_t index{}; index < capture.base_claim_rows.size(); ++index)
				if (capture.base_claim_rows[index].canonical_form() !=
					result.base_claim_rows()[index].canonical_form())
					return sdk::unexpected(artifact_error("task.capture.base", "binding"));
			for (std::size_t index{}; index < capture.source_span_claim_rows.size(); ++index)
				if (capture.source_span_claim_rows[index].canonical_form() !=
					result.source_span_claim_rows()[index].canonical_form())
					return sdk::unexpected(artifact_error("task.capture.spans", "binding"));
			if (capture.observation_rows.size() != result.observation_rows().size())
				return sdk::unexpected(artifact_error("task.capture.observations", "count"));
			for (std::size_t index{}; index < capture.observation_rows.size(); ++index)
			{
				const auto& left = capture.observation_rows[index];
				const auto& right = result.observation_rows()[index];
				if (left.batch_index != right.batch_index || left.row_index != right.row_index ||
					left.batch_index >= transcript.batches().size() ||
					left.row_index >= transcript.batches()[left.batch_index].rows().size())
					return sdk::unexpected(artifact_error("task.capture.observations", "index"));
				const auto observation_row_form =
					transcript.batches()[left.batch_index].rows()[left.row_index].canonical_form();
				if (left.observation_row_digest !=
						sdk::content_digest(std::as_bytes(
							std::span{observation_row_form.data(), observation_row_form.size()})) ||
					left.exact_equivalence != right.observation.exact_equivalence ||
					left.limitation != right.observation.limitation ||
					left.primary_span != right.observation.primary_span)
					return sdk::unexpected(artifact_error("task.capture.observations", "binding"));
			}
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_bundle_runtime_receipts(const materialization_prior_artifact_bundle& bundle,
										  const materialization_prior_artifact_limits& limits)
		{
			if (auto valid = validate_bundle(bundle, limits); !valid)
				return valid;
			for (const auto& task : bundle.tasks)
				if (task.capture.provider_execution_id.empty() ||
					task.capture.raw_frame_stream_bytes == 0U || task.capture.frame_count == 0U ||
					task.capture.raw_frame_stream.size() != task.capture.raw_frame_stream_bytes ||
					task.capture.raw_frame_stream.empty() ||
					sdk::content_digest(task.capture.raw_frame_stream) !=
						task.capture.raw_frame_stream_digest ||
					!canonical_content_digest(task.capture.raw_frame_stream_digest) ||
					!canonical_semantic_digest(task.capture.frame_transcript_digest) ||
					!canonical_semantic_digest(task.capture.sealed_transcript_digest))
					return sdk::unexpected(artifact_error("task.capture", "runtime-receipt"));
			return {};
		}

		[[nodiscard]] bool same_sealed_provider_transcript(
			const sdk::provider::detail::sealed_provider_transcript& left,
			const sdk::provider::detail::sealed_provider_transcript& right) noexcept
		{
			if (left.batches().size() != right.batches().size() ||
				left.coverage().size() != right.coverage().size() ||
				left.unresolved().size() != right.unresolved().size() ||
				left.evidence().size() != right.evidence().size())
				return false;
			for (std::size_t index{}; index < left.batches().size(); ++index)
			{
				const auto& a = left.batches()[index];
				const auto& b = right.batches()[index];
				if (a.task_id() != b.task_id() || a.descriptor_id() != b.descriptor_id() ||
					a.descriptor_digest() != b.descriptor_digest() ||
					a.dependency_group_id() != b.dependency_group_id() ||
					a.atomic_output_group_id() != b.atomic_output_group_id() ||
					a.batch_id() != b.batch_id() || a.batch_digest() != b.batch_digest() ||
					!std::ranges::equal(a.columns(), b.columns()) ||
					!std::ranges::equal(a.ordered_chunk_digests(), b.ordered_chunk_digests()) ||
					a.rows().size() != b.rows().size())
					return false;
				for (std::size_t row{}; row < a.rows().size(); ++row)
					if (a.rows()[row].canonical_form() != b.rows()[row].canonical_form())
						return false;
			}
			for (std::size_t index{}; index < left.coverage().size(); ++index)
			{
				const auto& a = left.coverage()[index];
				const auto& b = right.coverage()[index];
				if (a.kind != b.kind || a.id != b.id || a.state != b.state || a.reason != b.reason)
					return false;
			}
			return std::ranges::equal(left.unresolved(), right.unresolved()) &&
				std::ranges::equal(left.evidence(), right.evidence());
		}
	} // namespace

	sdk::result<std::vector<std::byte>>
	encode_materialization_prior_artifact(const materialization_prior_artifact_bundle& bundle,
										  const materialization_prior_artifact_limits& limits)
	{
		if (!valid_artifact_limits(limits))
			return sdk::unexpected(artifact_error("limits", "zero"));
		if (auto valid = validate_bundle_runtime_receipts(bundle, limits); !valid)
			return sdk::unexpected(std::move(valid.error()));
		try
		{
			std::vector<sdk::canonical_value> tasks;
			tasks.reserve(bundle.tasks.size());
			std::size_t total_capture_bytes{};
			for (const auto& task : bundle.tasks)
			{
				auto encoded = task_value(task, limits);
				if (!encoded)
					return sdk::unexpected(std::move(encoded.error()));
				auto capture =
					encode_detailed_task_report_capture(task.capture, capture_limits(limits));
				if (!capture || capture->size() > limits.max_capture_bytes ||
					capture->size() > limits.max_total_capture_bytes ||
					total_capture_bytes > limits.max_total_capture_bytes - capture->size())
					return sdk::unexpected(artifact_error("task.capture", "aggregate-limit"));
				total_capture_bytes += capture->size();
				tasks.push_back(std::move(*encoded));
			}
			auto publication = make_publication_value(bundle);
			if (!publication)
				return sdk::unexpected(std::move(publication.error()));
			auto body = sdk::canonical_binary(sdk::canonical_value::from_tuple({
				string_value(bundle.schema),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(bundle.version)),
				selector_value(bundle.selector),
				std::move(*publication),
				sdk::canonical_value::from_tuple(std::move(tasks)),
			}));
			if (!body)
				return sdk::unexpected(std::move(body.error()));
			const auto digest = sdk::content_digest(*body);
			auto envelope = sdk::canonical_binary(sdk::canonical_value::from_tuple({
				string_value(artifact_schema),
				sdk::canonical_value::from_bytes(std::move(*body)),
				string_value(digest),
			}));
			if (!envelope || envelope->size() > limits.max_bytes)
				return sdk::unexpected(artifact_error("envelope", "size-or-encode"));
			return envelope;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(artifact_error("envelope", "allocation"));
		}
	}

	sdk::result<materialization_prior_artifact_bundle>
	decode_materialization_prior_artifact(const std::span<const std::byte> artifact_bytes,
										  const materialization_prior_artifact_limits& limits)
	{
		if (!valid_artifact_limits(limits) || artifact_bytes.empty() ||
			artifact_bytes.size() > limits.max_bytes)
			return sdk::unexpected(artifact_error("envelope", "bounds"));
		try
		{
			auto decoded = sdk::canonical_binary_decode(artifact_bytes);
		if (!decoded)
			return sdk::unexpected(artifact_error("envelope", "canonical"));
		auto envelope_encoded = sdk::canonical_binary(*decoded);
		if (!envelope_encoded || envelope_encoded->size() != artifact_bytes.size() ||
			!std::ranges::equal(*envelope_encoded, artifact_bytes))
			return sdk::unexpected(artifact_error("envelope", "noncanonical"));
		auto envelope = tuple(*decoded, 3U, "envelope");
		if (!envelope)
			return sdk::unexpected(std::move(envelope.error()));
		auto schema = string((*envelope)[0U], "envelope.schema");
		auto body_bytes = bytes((*envelope)[1U], "envelope.body");
		auto expected_digest = string((*envelope)[2U], "envelope.digest");
		if (!schema || !body_bytes || !expected_digest || *schema != artifact_schema ||
			!canonical_content_digest(*expected_digest) ||
			sdk::content_digest(*body_bytes) != *expected_digest)
			return sdk::unexpected(artifact_error("envelope", "digest-or-schema"));
		auto body = sdk::canonical_binary_decode(*body_bytes);
		if (!body)
			return sdk::unexpected(artifact_error("body", "canonical"));
		auto body_encoded = sdk::canonical_binary(*body);
		if (!body_encoded || *body_encoded != *body_bytes)
			return sdk::unexpected(artifact_error("body", "noncanonical"));
		auto body_fields = tuple(*body, 5U, "body");
		if (!body_fields)
			return sdk::unexpected(std::move(body_fields.error()));
		auto body_schema = string((*body_fields)[0U], "body.schema");
		auto version = integer((*body_fields)[1U], "body.version");
		auto selector = decode_selector((*body_fields)[2U]);
		auto publication = decode_publication((*body_fields)[3U]);
		auto tasks = bounded_tuple((*body_fields)[4U], limits.max_tasks, "body.tasks");
		if (!body_schema || !version || !selector || !publication || !tasks ||
			*body_schema != artifact_schema || *version != artifact_version)
			return sdk::unexpected(artifact_error("body", "field"));
		materialization_prior_artifact_bundle output;
		output.schema = std::move(*body_schema);
		output.version = static_cast<std::uint32_t>(*version);
		output.selector = std::move(*selector);
		output.series_id = std::get<1>(*publication);
		output.publication_id = std::get<0>(*publication);
		output.snapshot_id = std::get<2>(*publication);
		output.sequence = std::get<3>(*publication);
		output.physical_generation = std::get<4>(*publication);
		output.parent_publication = std::get<5>(*publication);
		output.publication_state = std::get<6>(*publication);
		output.publication_corrupt = std::get<7>(*publication);
		output.tasks.reserve(tasks->size());
		std::size_t total_capture_bytes{};
		for (const auto& value : *tasks)
		{
			auto task = decode_task(value, limits);
			if (!task)
				return sdk::unexpected(std::move(task.error()));
			auto encoded_capture =
				encode_detailed_task_report_capture(task->capture, capture_limits(limits));
			if (!encoded_capture || encoded_capture->size() > limits.max_capture_bytes ||
				encoded_capture->size() > limits.max_total_capture_bytes ||
				total_capture_bytes > limits.max_total_capture_bytes - encoded_capture->size())
				return sdk::unexpected(artifact_error("task.capture", "aggregate-limit"));
			total_capture_bytes += encoded_capture->size();
			output.tasks.push_back(std::move(*task));
		}
		if (auto valid = validate_bundle_runtime_receipts(output, limits); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(artifact_error("envelope", "allocation"));
		}
	}

	sdk::result<std::optional<materialization_prior_artifact_bundle>>
	load_materialization_prior_artifact(const materialization_effect_root& root,
										const sdk::relation_engine& engine,
										const validated_publication_request& publication,
										const materialization_prior_artifact_limits& limits)
	{
		if (publication.backend == "memory")
		{
			if (!publication.genesis || publication.expected_parent_publication ||
				publication.sqlite_path)
				return sdk::unexpected(artifact_error("memory", "publication-policy"));
			std::vector<std::byte> bytes;
			try
			{
				auto& store = memory_store();
				std::lock_guard lock{store.mutex};
				auto found = store.artifacts.find(publication.series_id);
				if (found == store.artifacts.end())
					return std::optional<materialization_prior_artifact_bundle>{};
				if (found->second.size() > limits.max_bytes)
					return sdk::unexpected(artifact_error("memory", "bounds"));
				bytes = found->second;
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(artifact_error("memory", "allocation"));
			}
			auto decoded = decode_materialization_prior_artifact(bytes, limits);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			if (decoded->series_id != publication.series_id ||
				decoded->selector != publication.selector ||
				decoded->publication_state != sdk::publication_state::committed ||
				decoded->publication_corrupt)
				return sdk::unexpected(artifact_error("memory", "selector-or-state"));
			return std::optional<materialization_prior_artifact_bundle>{std::move(*decoded)};
		}
		if (publication.backend != "sqlite" || publication.genesis ||
			!publication.expected_parent_publication || !publication.sqlite_path)
			return std::optional<materialization_prior_artifact_bundle>{};
		auto path =
			sidecar_path(*publication.sqlite_path, *publication.expected_parent_publication);
		if (!path)
			return sdk::unexpected(std::move(path.error()));
		auto opened = root.open_beneath(*path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (!opened)
		{
			if (is_missing_sidecar(opened.error()))
				return std::optional<materialization_prior_artifact_bundle>{};
			return sdk::unexpected(std::move(opened.error()));
		}
		auto bytes = read_sidecar(*opened, limits);
		if (!bytes)
			return sdk::unexpected(std::move(bytes.error()));
		auto decoded = decode_materialization_prior_artifact(*bytes, limits);
		if (!decoded)
			return sdk::unexpected(std::move(decoded.error()));
		if (decoded->publication_id != *publication.expected_parent_publication ||
			decoded->series_id != publication.series_id ||
			decoded->selector != publication.selector)
			return sdk::unexpected(artifact_error("sidecar", "selector-or-parent"));
		auto rooted_opener = materialization_rooted_store_opener::create(root);
		if (!rooted_opener)
			return sdk::unexpected(std::move(rooted_opener.error()));
		auto store = (*rooted_opener)->open_sqlite(*publication.sqlite_path, engine);
		if (!store)
			return sdk::unexpected(artifact_error("sidecar", "store-open"));
		auto expected_record = store->open_publication(*publication.expected_parent_publication);
		if (!expected_record)
			return sdk::unexpected(artifact_error("sidecar", "parent-not-present"));
		auto current = store->current(publication.selector);
		if (!current)
			return sdk::unexpected(artifact_error("sidecar", "current-not-present"));
		auto snapshot = store->open(decoded->snapshot_id);
		if (!snapshot)
			return sdk::unexpected(artifact_error("sidecar", "snapshot-not-present"));
		const auto same_record = [&](const sdk::publication_record& record)
		{
			return record.publication_id == decoded->publication_id &&
				record.series_id == decoded->series_id &&
				record.snapshot_id == decoded->snapshot_id &&
				record.sequence == decoded->sequence &&
				record.physical_generation == decoded->physical_generation &&
				record.parent_publication == decoded->parent_publication &&
				record.state == decoded->publication_state &&
				record.corrupt == decoded->publication_corrupt;
		};
		if (!same_record(expected_record->publication()) || !same_record(current->publication()) ||
			!same_record(snapshot->publication()) ||
			expected_record->manifest() != current->manifest() ||
			expected_record->manifest() != snapshot->manifest())
			return sdk::unexpected(artifact_error("sidecar", "store-identity"));
		return std::optional<materialization_prior_artifact_bundle>{std::move(*decoded)};
	}

	sdk::result<void>
	persist_materialization_prior_artifact(const materialization_effect_root& root,
												   const validated_publication_request& publication,
												   const sdk::publication_record& committed_record,
												   const materialization_store_observation& observation,
												   std::vector<materialization_prior_artifact_task> tasks,
												   const materialization_prior_artifact_limits& limits)
		{
			if ((publication.backend != "memory" && publication.backend != "sqlite") ||
			(publication.backend == "sqlite" && !publication.sqlite_path) ||
			(publication.backend == "memory" &&
				 (!publication.genesis || publication.expected_parent_publication ||
				  publication.sqlite_path)) ||
				observation.backend != publication.backend ||
				observation.selector != publication.selector ||
				observation.series_id != publication.series_id ||
				observation.expected_parent_publication != publication.expected_parent_publication ||
				observation.writer_begin_call_count != 1U || !observation.publication_attempted ||
				observation.publish_call_count != 1U || observation.first_issue ||
				!observation.candidate_manifest || !observation.candidate_identity ||
				!observation.publish_returned_record ||
				*observation.publish_returned_record != committed_record ||
				observation.candidate_manifest->id != committed_record.snapshot_id ||
				observation.candidate_identity->publication_id != committed_record.publication_id ||
				observation.candidate_identity->series_id != committed_record.series_id ||
				observation.candidate_identity->snapshot_id != committed_record.snapshot_id ||
				observation.candidate_identity->sequence != committed_record.sequence ||
				observation.candidate_identity->parent_publication != committed_record.parent_publication ||
				committed_record.state != sdk::publication_state::committed ||
				committed_record.corrupt || committed_record.series_id != publication.series_id ||
				committed_record.publication_id.empty() ||
				committed_record.parent_publication != publication.expected_parent_publication)
			return sdk::unexpected(artifact_error("sidecar", "publication"));
		materialization_prior_artifact_bundle bundle;
		bundle.selector = publication.selector;
		bundle.series_id = committed_record.series_id;
		bundle.publication_id = committed_record.publication_id;
		bundle.snapshot_id = committed_record.snapshot_id;
		bundle.sequence = committed_record.sequence;
		bundle.physical_generation = committed_record.physical_generation;
		bundle.parent_publication = committed_record.parent_publication;
		bundle.publication_state = committed_record.state;
		bundle.publication_corrupt = committed_record.corrupt;
		bundle.tasks = std::move(tasks);
		auto encoded = encode_materialization_prior_artifact(bundle, limits);
		if (!encoded)
			return sdk::unexpected(std::move(encoded.error()));
		if (publication.backend == "memory")
		{
			try
			{
				auto& store = memory_store();
				std::lock_guard lock{store.mutex};
				auto found = store.artifacts.find(publication.series_id);
				const auto previous_bytes =
					found == store.artifacts.end() ? 0U : found->second.size();
				if ((found == store.artifacts.end() &&
					 store.artifacts.size() >= max_memory_artifacts) ||
					encoded->size() > max_memory_artifact_bytes ||
					previous_bytes > store.resident_bytes ||
					store.resident_bytes - previous_bytes >
						max_memory_artifact_bytes - encoded->size())
					return sdk::unexpected(artifact_error("memory", "capacity"));
				const auto new_bytes = encoded->size();
				store.artifacts.insert_or_assign(publication.series_id, std::move(*encoded));
				store.resident_bytes = store.resident_bytes - previous_bytes + new_bytes;
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(artifact_error("memory", "allocation"));
			}
			return {};
		}
		auto path = sidecar_path(*publication.sqlite_path, committed_record.publication_id);
		if (!path)
			return sdk::unexpected(std::move(path.error()));
		return install_sidecar(root, *path, *encoded, limits);
	}

	sdk::result<sealed_materialization_result> rehydrate_materialization_prior_artifact(
		const materialization_prior_artifact_task& artifact,
		const std::size_t request_task_index,
		const validated_task_request& current_task,
		const sdk::provider::manifest& provider_manifest,
		const std::span<const sdk::relation_descriptor> output_descriptors,
		const sdk::provider::protocol_credit output_credit,
		const sdk::provider::protocol_limits protocol_limits,
		const detailed_report_limits& report_limits)
	{
		auto bounded_capture = encode_detailed_task_report_capture(artifact.capture, report_limits);
		if (!bounded_capture)
			return sdk::unexpected(artifact_error("task.capture", "report-limits"));
		if (artifact.identity.canonical_task_ordinal != request_task_index ||
			artifact.identity.provider_task_id != current_task.provider_task_id ||
			artifact.identity.task_input_digest != current_task.task_input_digest ||
			artifact.identity.selected_catalog_compile_unit_id !=
				current_task.worker_input.selected_catalog_compile_unit ||
			artifact.identity.final_relation_compile_unit_id !=
				current_task.worker_input.compile_unit ||
			artifact.capture.provider_execution_id != current_task.provider_execution_id)
			return sdk::unexpected(artifact_error("task", "current-identity"));
		const auto& input = current_task.worker_input;
		if (artifact.capture.project_id != input.project ||
			artifact.capture.catalog_id != input.project_catalog.catalog_id ||
			artifact.capture.catalog_digest != input.project_catalog.catalog_digest ||
			artifact.capture.selected_catalog_compile_unit_id !=
				input.selected_catalog_compile_unit ||
			artifact.capture.compile_unit_id != input.compile_unit ||
			artifact.capture.variant_id != input.variant ||
			artifact.capture.toolchain_context_id != input.toolchain_context ||
			artifact.capture.toolchain_digest != input.toolchain_digest ||
			artifact.capture.source_snapshot_id != input.source_snapshot ||
			artifact.capture.source_file_id != input.file ||
			artifact.capture.source_logical_path != input.logical_path ||
			artifact.capture.source_content_digest != input.source_content_digest ||
			artifact.capture.source_size_bytes != input.source_size_bytes ||
			artifact.capture.source_encoding != input.source_encoding ||
			artifact.capture.source_line_index_id != input.line_index ||
			artifact.capture.source_read_only != input.source_read_only ||
			artifact.capture.condition_universe_id != input.condition_universe ||
			artifact.capture.condition_id != input.condition ||
			artifact.capture.interpretation_domain != input.interpretation)
			return sdk::unexpected(artifact_error("task.capture", "current-context"));
		if (current_task.source_receipt &&
			(artifact.capture.source_size_bytes != current_task.source_receipt->size_bytes ||
			 artifact.capture.source_content_digest !=
				 current_task.source_receipt->content_digest ||
			 artifact.capture.source_line_index_id != current_task.source_receipt->line_index_id))
			return sdk::unexpected(artifact_error("task.capture", "source-receipt"));
		if (artifact.capture.raw_frame_stream.empty() ||
			artifact.capture.raw_frame_stream.size() != artifact.capture.raw_frame_stream_bytes ||
			sdk::content_digest(artifact.capture.raw_frame_stream) !=
				artifact.capture.raw_frame_stream_digest)
			return sdk::unexpected(artifact_error("task.capture", "raw-frame-stream"));
		auto frames = sdk::provider::decode_frame_stream(artifact.capture.raw_frame_stream,
												 protocol_limits);
		if (!frames || frames->size() != artifact.capture.frame_count)
			return sdk::unexpected(artifact_error("task.capture", "frame-stream-decode"));
		auto frame_digest =
			sdk::provider::detail::provider_frame_transcript_receipt_digest(*frames);
		if (!frame_digest || *frame_digest != artifact.capture.frame_transcript_digest)
			return sdk::unexpected(artifact_error("task.capture", "frame-transcript"));
		const sdk::provider::detail::transcript_validation_request raw_validation_request{
			current_task.provider_task_id,
			provider_manifest.provider_id,
			provider_manifest.provider_version,
			&provider_manifest,
			output_descriptors,
			output_credit,
			&current_task.worker_input.budget,
			true,
			nullptr,
		};
		auto raw_validation = sdk::provider::detail::validate_provider_transcript(
			raw_validation_request, *frames, protocol_limits);
		if (!raw_validation || raw_validation->kind !=
					sdk::provider::detail::transcript_terminal_kind::complete ||
			!raw_validation->sealed())
			return sdk::unexpected(artifact_error("task.capture", "raw-semantic-reproof"));
		try
		{
			std::vector<sdk::provider::detail::sealed_provider_batch_replay> batches;
			batches.reserve(artifact.capture.batches.size());
			for (const auto& source : artifact.capture.batches)
			{
				auto row_set = recompute_row_set_digest(source);
				if (!row_set || source.row_set_digest != *row_set)
					return sdk::unexpected(artifact_error("task.capture", "row-set-digest"));
				sdk::provider::detail::sealed_provider_batch_replay batch{
					source.task_id,
					source.descriptor_id,
					source.descriptor_digest,
					source.dependency_group_id,
					source.atomic_output_group_id,
					source.batch_id,
					source.batch_digest,
					source.columns,
					source.ordered_chunk_digests,
					{}};
				batch.rows.reserve(source.rows.size());
				for (const auto& row : source.rows)
				{
					auto decoded = decode_row(row.row_canonical_form);
					if (!decoded)
						return sdk::unexpected(std::move(decoded.error()));
					batch.rows.push_back(std::move(*decoded));
				}
				batches.push_back(std::move(batch));
			}
			auto transcript = sdk::provider::detail::rehydrate_provider_transcript(
				std::string{current_task.provider_task_id},
				output_descriptors,
				std::move(batches),
				[&]
				{
					std::vector<sdk::provider::coverage_unit> output;
					output.reserve(artifact.capture.coverage.size());
					for (const auto& item : artifact.capture.coverage)
						output.push_back({item.kind, item.id, item.state, item.reason});
					return output;
				}(),
				[&]
				{
					std::vector<sdk::provider::unresolved_item> output;
					output.reserve(artifact.capture.unresolved.size());
					for (const auto& item : artifact.capture.unresolved)
						output.push_back({item.code, item.subject, item.detail});
					return output;
				}(),
				[&]
				{
					std::vector<sdk::provider::evidence_item> output;
					output.reserve(artifact.capture.evidence.size());
					for (const auto& item : artifact.capture.evidence)
						output.push_back({item.kind, item.subject, item.producer, item.summary});
					return output;
				}());
			if (!transcript)
				return sdk::unexpected(std::move(transcript.error()));
			if (!same_sealed_provider_transcript(*raw_validation->sealed(), *transcript))
				return sdk::unexpected(artifact_error("task.capture", "raw-semantic-binding"));
			auto result = seal_validated_provider_result(current_task.worker_input,
															 current_task.provider_task_id,
														 current_task.task_input_digest,
														 current_task.provider_execution_id,
														 std::move(*transcript));
			if (!result)
				return sdk::unexpected(std::move(result.error()));
			if (!same_sealed_provider_transcript(*raw_validation->sealed(), result->provider_seal()))
				return sdk::unexpected(artifact_error("task.capture", "raw-semantic-binding"));
			auto digest = seal_materialization_incremental_artifact_digest(*result);
			if (!digest || *digest != artifact.sealed_artifact_digest)
				return sdk::unexpected(artifact_error("task", "sealed-artifact-digest"));
			if (auto capture_valid = validate_capture_against_result(artifact.capture, *result);
				!capture_valid)
				return sdk::unexpected(std::move(capture_valid.error()));
			auto sealed_receipt = sdk::provider::detail::provider_sealed_transcript_receipt_digest(
				current_task.provider_task_id, "provider.success", result->provider_seal());
			if (!sealed_receipt || artifact.capture.sealed_transcript_digest != *sealed_receipt)
				return sdk::unexpected(artifact_error("task.capture", "sealed-receipt"));
			return result;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(artifact_error("task", "allocation"));
		}
	}
} // namespace cxxlens::detail::clang22::materialization
