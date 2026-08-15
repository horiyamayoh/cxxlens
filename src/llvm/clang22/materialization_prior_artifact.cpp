#include "materialization_prior_artifact.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <limits>
#include <map>
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
		std::atomic<std::uint64_t> sidecar_attempt_counter{};

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

		[[nodiscard]] bool canonical_provider_execution_id(const std::string_view value) noexcept
		{
			constexpr std::string_view prefix{"provider-execution:sha256:"};
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

		class materialization_spool_cursor
		{
		  public:
			materialization_spool_cursor(materialization_replayable_spool& source,
										 const std::uint64_t begin,
										 const std::uint64_t end) noexcept
				: source_{source}, offset_{begin}, end_{end}
			{
			}

			[[nodiscard]] std::uint64_t offset() const noexcept
			{
				return offset_;
			}

			[[nodiscard]] std::uint64_t end() const noexcept
			{
				return end_;
			}

			[[nodiscard]] sdk::result<std::uint8_t> read_byte(const std::string_view field)
			{
				std::array<std::byte, 1U> bytes{};
				if (auto read = read_into(bytes, field); !read)
					return sdk::unexpected(std::move(read.error()));
				return std::to_integer<std::uint8_t>(bytes.front());
			}

			[[nodiscard]] sdk::result<std::uint64_t> read_length(const std::string_view field)
			{
				std::array<std::byte, 8U> bytes{};
				if (auto read = read_into(bytes, field); !read)
					return sdk::unexpected(std::move(read.error()));
				std::uint64_t value{};
				for (const auto byte : bytes)
					value = (value << 8U) | std::to_integer<std::uint8_t>(byte);
				return value;
			}

			[[nodiscard]] sdk::result<std::vector<std::byte>>
			read_item(const std::string_view field, const std::uint64_t maximum)
			{
				auto length = read_length(field);
				if (!length || *length == 0U || *length > maximum || *length > remaining())
					return sdk::unexpected(artifact_error(std::string{field}, "item-length"));
				if (*length > std::numeric_limits<std::size_t>::max())
					return sdk::unexpected(artifact_error(std::string{field}, "item-size"));
				try
				{
					std::vector<std::byte> output(static_cast<std::size_t>(*length));
					if (auto read = read_into(output, field); !read)
						return sdk::unexpected(std::move(read.error()));
					return output;
				}
				catch (const std::bad_alloc&)
				{
					return sdk::unexpected(artifact_error(std::string{field}, "allocation"));
				}
			}

			[[nodiscard]] sdk::result<void> copy_to(materialization_private_spool& target,
													const std::uint64_t count,
													const std::string_view field)
			{
				if (count > remaining())
					return sdk::unexpected(artifact_error(std::string{field}, "payload-length"));
				try
				{
					std::vector<std::byte> buffer(default_stream_chunk_bytes);
					std::uint64_t copied{};
					while (copied < count)
					{
						const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
							count - copied, static_cast<std::uint64_t>(buffer.size())));
						if (auto read = read_into(std::span{buffer}.first(chunk), field); !read)
							return read;
						if (auto appended = target.append(std::span{buffer}.first(chunk));
							!appended)
							return sdk::unexpected(
								artifact_error(std::string{field}, "spool-write"));
						copied += chunk;
					}
				}
				catch (const std::bad_alloc&)
				{
					return sdk::unexpected(artifact_error(std::string{field}, "allocation"));
				}
				return {};
			}

		  private:
			[[nodiscard]] std::uint64_t remaining() const noexcept
			{
				return offset_ <= end_ ? end_ - offset_ : 0U;
			}

			[[nodiscard]] sdk::result<void> read_into(const std::span<std::byte> destination,
													  const std::string_view field)
			{
				if (destination.size() > remaining())
					return sdk::unexpected(artifact_error(std::string{field}, "truncated"));
				std::size_t received{};
				while (received < destination.size())
				{
					auto read = source_.read_at(offset_, destination.subspan(received));
					if (!read || *read == 0U || *read > destination.size() - received)
						return sdk::unexpected(artifact_error(std::string{field}, "spool-read"));
					received += *read;
					offset_ += *read;
				}
				return {};
			}

			materialization_replayable_spool& source_;
			std::uint64_t offset_{};
			std::uint64_t end_{};
		};

		[[nodiscard]] sdk::result<std::unique_ptr<materialization_replayable_spool>>
		spool_sidecar(const materialization_owned_fd& file,
					  const materialization_prior_artifact_limits& limits)
		{
			auto identity = materialization_fd_identity(file.get(), true);
			if (!identity || identity->size_bytes == 0U || identity->size_bytes > limits.max_bytes)
				return sdk::unexpected(artifact_error("sidecar", "size"));
			auto storage = make_materialization_private_spool();
			if (!storage)
				return sdk::unexpected(artifact_error("sidecar", "spool-create"));
			try
			{
				std::vector<std::byte> buffer(default_stream_chunk_bytes);
				std::uint64_t offset{};
				while (offset < identity->size_bytes)
				{
					const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
						identity->size_bytes - offset, static_cast<std::uint64_t>(buffer.size())));
					std::size_t received{};
					while (received < chunk)
					{
						const auto count =
							::read(file.get(), buffer.data() + received, chunk - received);
						if (count < 0 && errno == EINTR)
							continue;
						if (count <= 0 || static_cast<std::size_t>(count) > chunk - received)
							return sdk::unexpected(artifact_error("sidecar", "read"));
						received += static_cast<std::size_t>(count);
					}
					if (auto appended = (*storage)->append(std::span{buffer}.first(chunk));
						!appended)
						return sdk::unexpected(artifact_error("sidecar", "spool-write"));
					offset += chunk;
				}
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(artifact_error("sidecar", "allocation"));
			}
			auto final_identity = materialization_fd_identity(file.get(), true);
			if (!final_identity || *final_identity != *identity)
				return sdk::unexpected(artifact_error("sidecar", "immutable-conflict"));
			if (auto sealed = (*storage)->seal(); !sealed)
				return sdk::unexpected(artifact_error("sidecar", "spool-seal"));
			return std::move(*storage);
		}

		[[nodiscard]] sdk::result<void> write_all(const materialization_owned_fd& file,
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
			return {};
		}

		[[nodiscard]] sdk::result<void> sync_sidecar_parent(const materialization_effect_root& root,
															const std::string_view path);

		[[nodiscard]] sdk::result<void>
		compare_sidecar_to_spool(const materialization_owned_fd& file,
								 materialization_replayable_spool& spool,
								 const materialization_prior_artifact_limits& limits)
		{
			auto identity = materialization_fd_identity(file.get(), true);
			if (!identity || identity->size_bytes != spool.size_bytes() ||
				identity->size_bytes > limits.max_bytes)
				return sdk::unexpected(artifact_error("sidecar", "immutable-conflict"));
			try
			{
				std::vector<std::byte> expected(default_stream_chunk_bytes);
				std::vector<std::byte> actual(default_stream_chunk_bytes);
				std::uint64_t offset{};
				while (offset < spool.size_bytes())
				{
					const auto remaining = spool.size_bytes() - offset;
					const auto chunk = static_cast<std::size_t>(
						std::min<std::uint64_t>(remaining, expected.size()));
					std::size_t received{};
					while (received < chunk)
					{
						const auto count =
							::read(file.get(), actual.data() + received, chunk - received);
						if (count < 0 && errno == EINTR)
							continue;
						if (count <= 0 || static_cast<std::size_t>(count) > chunk - received)
							return sdk::unexpected(artifact_error("sidecar", "read"));
						received += static_cast<std::size_t>(count);
					}
					auto read = spool.read_at(offset, std::span{expected}.first(chunk));
					if (!read || *read != chunk ||
						!std::ranges::equal(std::span{expected}.first(chunk),
											std::span{actual}.first(chunk)))
						return sdk::unexpected(artifact_error("sidecar", "immutable-conflict"));
					offset += chunk;
				}
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(artifact_error("sidecar", "allocation"));
			}
			return {};
		}

		[[nodiscard]] sdk::result<void> write_spool_sidecar(const materialization_owned_fd& file,
															materialization_replayable_spool& spool)
		{
			try
			{
				std::vector<std::byte> buffer(default_stream_chunk_bytes);
				std::uint64_t offset{};
				while (offset < spool.size_bytes())
				{
					const auto remaining = spool.size_bytes() - offset;
					const auto chunk =
						static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
					auto read = spool.read_at(offset, std::span{buffer}.first(chunk));
					if (!read || *read != chunk)
						return sdk::unexpected(artifact_error("sidecar", "spool-read"));
					if (auto written = write_all(file, std::span{buffer}.first(chunk)); !written)
						return written;
					offset += chunk;
				}
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(artifact_error("sidecar", "allocation"));
			}
			if (::fsync(file.get()) != 0)
				return sdk::unexpected(artifact_error("sidecar", "sync"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		install_sidecar_spool(const materialization_effect_root& root,
							  const std::string_view path,
							  materialization_replayable_spool& spool,
							  const materialization_prior_artifact_limits& limits)
		{
			auto existing = root.open_beneath(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (existing)
				return compare_sidecar_to_spool(*existing, spool, limits);
			if (!is_missing_sidecar(existing.error()))
				return sdk::unexpected(std::move(existing.error()));

			if (!spool.sealed() || spool.size_bytes() == 0U ||
				spool.size_bytes() > limits.max_bytes)
				return sdk::unexpected(artifact_error("sidecar", "spool-lifecycle"));
			auto payload_digest = digest_materialization_spool(spool);
			if (!payload_digest || payload_digest->rfind("sha256:", 0U) != 0U ||
				payload_digest->size() <= 7U)
				return sdk::unexpected(artifact_error("sidecar", "payload-digest"));
			const auto attempt = sidecar_attempt_counter.fetch_add(1U, std::memory_order_relaxed);
			std::string temporary_path{path};
			std::string temporary_suffix{".tmp-"};
			temporary_suffix.append(payload_digest->substr(7U));
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
				if (auto written = write_spool_sidecar(*temporary, spool); !written)
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
					if (auto matching = compare_sidecar_to_spool(*raced, spool, limits); !matching)
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
				if (!canonical_provider_execution_id(task.capture.provider_execution_id) ||
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

		[[nodiscard]] bool same_detached_cell(const sdk::detached_cell& left,
											  const sdk::detached_cell& right)
		{
			return left.type == right.type && left.state == right.state &&
				left.value == right.value && left.unknown_reason == right.unknown_reason;
		}

		[[nodiscard]] bool same_detached_rows(const std::vector<sdk::detached_row>& left,
											  const std::vector<sdk::detached_row>& right)
		{
			if (left.size() != right.size())
				return false;
			for (std::size_t index{}; index < left.size(); ++index)
			{
				if (left[index].descriptor_id != right[index].descriptor_id ||
					left[index].cells.size() != right[index].cells.size())
					return false;
				auto left_cell = left[index].cells.begin();
				auto right_cell = right[index].cells.begin();
				for (; left_cell != left[index].cells.end(); ++left_cell, ++right_cell)
					if (left_cell->first != right_cell->first ||
						!same_detached_cell(left_cell->second, right_cell->second))
						return false;
			}
			return true;
		}

		[[nodiscard]] bool same_capture(const detailed_task_report_capture& left,
										const detailed_task_report_capture& right)
		{
			if (std::tie(left.provider_task_id,
						 left.provider_execution_id,
						 left.project_id,
						 left.catalog_id,
						 left.catalog_digest,
						 left.selected_catalog_compile_unit_id,
						 left.compile_unit_id,
						 left.variant_id,
						 left.toolchain_context_id,
						 left.toolchain_digest,
						 left.source_snapshot_id,
						 left.source_file_id,
						 left.source_logical_path,
						 left.source_content_digest,
						 left.source_size_bytes,
						 left.source_encoding,
						 left.source_line_index_id,
						 left.source_read_only,
						 left.task_input_digest,
						 left.condition_universe_id,
						 left.condition_id,
						 left.interpretation_domain,
						 left.input_protocol_major,
						 left.input_protocol_minor,
						 left.logical_input_bytes,
						 left.canonical_chunk_bytes,
						 left.input_chunk_count,
						 left.ordered_chunk_payload_digest_set_digest,
						 left.raw_frame_stream_bytes,
						 left.raw_frame_stream,
						 left.raw_frame_stream_digest,
						 left.frame_count,
						 left.frame_transcript_digest,
						 left.sealed_transcript_digest,
						 left.capture_binding_digest) !=
				std::tie(right.provider_task_id,
						 right.provider_execution_id,
						 right.project_id,
						 right.catalog_id,
						 right.catalog_digest,
						 right.selected_catalog_compile_unit_id,
						 right.compile_unit_id,
						 right.variant_id,
						 right.toolchain_context_id,
						 right.toolchain_digest,
						 right.source_snapshot_id,
						 right.source_file_id,
						 right.source_logical_path,
						 right.source_content_digest,
						 right.source_size_bytes,
						 right.source_encoding,
						 right.source_line_index_id,
						 right.source_read_only,
						 right.task_input_digest,
						 right.condition_universe_id,
						 right.condition_id,
						 right.interpretation_domain,
						 right.input_protocol_major,
						 right.input_protocol_minor,
						 right.logical_input_bytes,
						 right.canonical_chunk_bytes,
						 right.input_chunk_count,
						 right.ordered_chunk_payload_digest_set_digest,
						 right.raw_frame_stream_bytes,
						 right.raw_frame_stream,
						 right.raw_frame_stream_digest,
						 right.frame_count,
						 right.frame_transcript_digest,
						 right.sealed_transcript_digest,
						 right.capture_binding_digest))
				return false;
			if (left.ordered_chunk_digests != right.ordered_chunk_digests ||
				left.coverage.size() != right.coverage.size() ||
				left.unresolved.size() != right.unresolved.size() ||
				left.evidence.size() != right.evidence.size() ||
				left.batches.size() != right.batches.size() ||
				left.observation_rows.size() != right.observation_rows.size())
				return false;
			for (std::size_t index{}; index < left.coverage.size(); ++index)
				if (std::tie(left.coverage[index].kind,
							 left.coverage[index].id,
							 left.coverage[index].state,
							 left.coverage[index].reason) !=
					std::tie(right.coverage[index].kind,
							 right.coverage[index].id,
							 right.coverage[index].state,
							 right.coverage[index].reason))
					return false;
			for (std::size_t index{}; index < left.unresolved.size(); ++index)
				if (std::tie(left.unresolved[index].code,
							 left.unresolved[index].subject,
							 left.unresolved[index].detail) !=
					std::tie(right.unresolved[index].code,
							 right.unresolved[index].subject,
							 right.unresolved[index].detail))
					return false;
			for (std::size_t index{}; index < left.evidence.size(); ++index)
				if (std::tie(left.evidence[index].kind,
							 left.evidence[index].subject,
							 left.evidence[index].producer,
							 left.evidence[index].summary) !=
					std::tie(right.evidence[index].kind,
							 right.evidence[index].subject,
							 right.evidence[index].producer,
							 right.evidence[index].summary))
					return false;
			for (std::size_t index{}; index < left.batches.size(); ++index)
			{
				const auto& a = left.batches[index];
				const auto& b = right.batches[index];
				if (std::tie(a.task_id,
							 a.descriptor_id,
							 a.descriptor_digest,
							 a.dependency_group_id,
							 a.atomic_output_group_id,
							 a.batch_id,
							 a.batch_digest,
							 a.columns,
							 a.ordered_chunk_digests,
							 a.row_count,
							 a.row_set_digest) !=
						std::tie(b.task_id,
								 b.descriptor_id,
								 b.descriptor_digest,
								 b.dependency_group_id,
								 b.atomic_output_group_id,
								 b.batch_id,
								 b.batch_digest,
								 b.columns,
								 b.ordered_chunk_digests,
								 b.row_count,
								 b.row_set_digest) ||
					a.rows.size() != b.rows.size())
					return false;
				for (std::size_t row{}; row < a.rows.size(); ++row)
					if (std::tie(a.rows[row].row_index,
								 a.rows[row].row_canonical_form,
								 a.rows[row].row_digest) !=
						std::tie(b.rows[row].row_index,
								 b.rows[row].row_canonical_form,
								 b.rows[row].row_digest))
						return false;
			}
			for (std::size_t index{}; index < left.observation_rows.size(); ++index)
			{
				const auto& a = left.observation_rows[index];
				const auto& b = right.observation_rows[index];
				if (std::tie(a.batch_index,
							 a.row_index,
							 a.observation_row_digest,
							 a.exact_equivalence,
							 a.limitation,
							 a.primary_span) !=
					std::tie(b.batch_index,
							 b.row_index,
							 b.observation_row_digest,
							 b.exact_equivalence,
							 b.limitation,
							 b.primary_span))
					return false;
			}
			return same_detached_rows(left.base_claim_rows, right.base_claim_rows) &&
				same_detached_rows(left.source_span_claim_rows, right.source_span_claim_rows);
		}

		void append_big_endian_length(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (int shift = 56; shift >= 0; shift -= 8)
				output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
		}

		[[nodiscard]] sdk::result<void> append_canonical_field(std::vector<std::byte>& output,
															   const sdk::canonical_value& value)
		{
			auto encoded = sdk::canonical_binary(value);
			if (!encoded)
				return sdk::unexpected(artifact_error("envelope", "canonical"));
			append_big_endian_length(output, encoded->size());
			output.insert(output.end(), encoded->begin(), encoded->end());
			return {};
		}

		[[nodiscard]] sdk::result<void> append_spool_bytes(materialization_private_spool& target,
														   const std::span<const std::byte> bytes,
														   const std::string_view field)
		{
			if (auto appended = target.append(bytes); !appended)
				return sdk::unexpected(artifact_error(std::string{field}, "spool-io"));
			return {};
		}

		[[nodiscard]] sdk::result<void> append_spool_copy(materialization_private_spool& target,
														  materialization_replayable_spool& source,
														  const std::string_view field)
		{
			if (!source.sealed())
				return sdk::unexpected(artifact_error(std::string{field}, "spool-lifecycle"));
			try
			{
				std::vector<std::byte> buffer(default_stream_chunk_bytes);
				std::uint64_t offset{};
				while (offset < source.size_bytes())
				{
					const auto remaining = source.size_bytes() - offset;
					const auto chunk =
						static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
					auto read = source.read_at(offset, std::span{buffer}.first(chunk));
					if (!read || *read != chunk)
						return sdk::unexpected(artifact_error(std::string{field}, "spool-read"));
					if (auto appended =
							append_spool_bytes(target, std::span{buffer}.first(chunk), field);
						!appended)
						return appended;
					offset += chunk;
				}
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(artifact_error(std::string{field}, "allocation"));
			}
			return {};
		}

		[[nodiscard]] sdk::result<void> seal_artifact_spool(materialization_replayable_spool& spool,
															const std::string_view field)
		{
			if (auto sealed = spool.seal(); !sealed)
				return sdk::unexpected(artifact_error(std::string{field}, "spool-seal"));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_prior_artifact_task_metadata(
			const materialization_prior_artifact_task_metadata& task,
			const std::size_t previous_ordinal,
			const bool first)
		{
			if ((first && task.identity.canonical_task_ordinal != 0U) ||
				(!first &&
				 (task.identity.canonical_task_ordinal <= previous_ordinal ||
				  task.identity.canonical_task_ordinal != previous_ordinal + 1U)) ||
				!sdk::validate_strong_id(task.identity.provider_task_id) ||
				!sdk::validate_strong_id(task.identity.task_input_digest) ||
				!sdk::validate_strong_id(task.identity.selected_catalog_compile_unit_id) ||
				!sdk::validate_strong_id(task.identity.final_relation_compile_unit_id) ||
				!canonical_artifact_digest(task.sealed_artifact_digest) ||
				!canonical_provider_execution_id(task.provider_execution_id))
				return sdk::unexpected(artifact_error("bundle.tasks", "identity-or-order"));
			if (auto valid = task.state.validate(); !valid)
				return sdk::unexpected(artifact_error("bundle.tasks.state", "validation"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_prior_artifact_capture_runtime_receipt(const detailed_task_report_capture& capture)
		{
			if (!canonical_provider_execution_id(capture.provider_execution_id) ||
				capture.raw_frame_stream_bytes == 0U || capture.frame_count == 0U ||
				capture.raw_frame_stream.size() != capture.raw_frame_stream_bytes ||
				capture.raw_frame_stream.empty() ||
				sdk::content_digest(capture.raw_frame_stream) != capture.raw_frame_stream_digest ||
				!canonical_content_digest(capture.raw_frame_stream_digest) ||
				!canonical_semantic_digest(capture.frame_transcript_digest) ||
				!canonical_semantic_digest(capture.sealed_transcript_digest))
				return sdk::unexpected(artifact_error("task.capture", "runtime-receipt"));
			return {};
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		decode_spool_item(materialization_spool_cursor& cursor,
						  const std::string_view field,
						  const std::uint64_t maximum)
		{
			auto encoded = cursor.read_item(field, maximum);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			auto decoded = sdk::canonical_binary_decode(*encoded);
			if (!decoded)
				return sdk::unexpected(artifact_error(std::string{field}, "canonical"));
			return std::move(*decoded);
		}

		[[nodiscard]] detailed_report_limits
		replay_capture_limits(const materialization_prior_artifact_limits& limits) noexcept
		{
			auto output = capture_limits(limits);
			output.max_tasks = limits.max_tasks;
			output.max_projection_bytes =
				std::min(limits.max_bytes, limits.max_total_capture_bytes);
			return output;
		}

		[[nodiscard]] sdk::result<materialization_prior_artifact_replay_bundle>
		decode_streamed_prior_artifact(materialization_replayable_spool& sidecar,
									   const materialization_prior_artifact_limits& limits)
		{
			if (!sidecar.sealed() || sidecar.size_bytes() == 0U ||
				sidecar.size_bytes() > limits.max_bytes)
				return sdk::unexpected(artifact_error("sidecar", "spool-lifecycle"));
			materialization_spool_cursor envelope{sidecar, 0U, sidecar.size_bytes()};
			auto envelope_tag = envelope.read_byte("envelope.tag");
			auto envelope_count = envelope.read_length("envelope.count");
			if (!envelope_tag || !envelope_count || *envelope_tag != 0x05U || *envelope_count != 3U)
				return sdk::unexpected(artifact_error("envelope", "tuple-shape"));
			auto schema_value = decode_spool_item(envelope, "envelope.schema", limits.max_bytes);
			if (!schema_value)
				return sdk::unexpected(std::move(schema_value.error()));
			auto schema = string(*schema_value, "envelope.schema");
			if (!schema || *schema != artifact_schema)
				return sdk::unexpected(artifact_error("envelope.schema", "value"));

			auto body_item_length = envelope.read_length("envelope.body");
			if (!body_item_length || *body_item_length < 9U ||
				*body_item_length > envelope.end() - envelope.offset())
				return sdk::unexpected(artifact_error("envelope.body", "item-length"));
			const auto body_item_end = envelope.offset() + *body_item_length;
			auto body_tag = envelope.read_byte("envelope.body");
			auto body_size = envelope.read_length("envelope.body");
			if (!body_tag || !body_size || *body_tag != 0x03U || *body_size > limits.max_bytes ||
				*body_size != *body_item_length - 9U)
				return sdk::unexpected(artifact_error("envelope.body", "bytes-shape"));
			auto body_storage = make_materialization_private_spool();
			if (!body_storage)
				return sdk::unexpected(artifact_error("envelope.body", "spool-create"));
			if (auto copied = envelope.copy_to(**body_storage, *body_size, "envelope.body");
				!copied)
				return sdk::unexpected(std::move(copied.error()));
			if (envelope.offset() != body_item_end)
				return sdk::unexpected(artifact_error("envelope.body", "trailing-bytes"));
			if (auto sealed = (*body_storage)->seal(); !sealed)
				return sdk::unexpected(artifact_error("envelope.body", "spool-seal"));

			auto digest_value = decode_spool_item(envelope, "envelope.digest", limits.max_bytes);
			if (!digest_value)
				return sdk::unexpected(std::move(digest_value.error()));
			auto expected_digest = string(*digest_value, "envelope.digest");
			if (!expected_digest || !canonical_content_digest(*expected_digest) ||
				envelope.offset() != envelope.end())
				return sdk::unexpected(artifact_error("envelope", "canonical"));
			auto actual_digest = digest_materialization_spool(**body_storage);
			if (!actual_digest || *actual_digest != *expected_digest)
				return sdk::unexpected(artifact_error("envelope.digest", "body-mismatch"));

			materialization_spool_cursor body{**body_storage, 0U, (*body_storage)->size_bytes()};
			auto body_tag_value = body.read_byte("body.tag");
			auto body_count = body.read_length("body.count");
			if (!body_tag_value || !body_count || *body_tag_value != 0x05U || *body_count != 5U)
				return sdk::unexpected(artifact_error("body", "tuple-shape"));
			auto body_schema_value = decode_spool_item(body, "body.schema", limits.max_bytes);
			auto body_version_value = decode_spool_item(body, "body.version", limits.max_bytes);
			auto selector_value_decoded =
				decode_spool_item(body, "body.selector", limits.max_bytes);
			auto publication_value_decoded =
				decode_spool_item(body, "body.publication", limits.max_bytes);
			if (!body_schema_value || !body_version_value || !selector_value_decoded ||
				!publication_value_decoded)
				return sdk::unexpected(artifact_error("body", "field"));
			auto body_schema = string(*body_schema_value, "body.schema");
			auto body_version = integer(*body_version_value, "body.version");
			auto selector = decode_selector(*selector_value_decoded);
			auto publication = decode_publication(*publication_value_decoded);
			if (!body_schema || *body_schema != artifact_schema || !body_version ||
				*body_version != artifact_version || !selector || !publication)
				return sdk::unexpected(artifact_error("body", "authority"));

			auto task_item_length = body.read_length("body.tasks");
			if (!task_item_length || *task_item_length == 0U ||
				*task_item_length > body.end() - body.offset())
				return sdk::unexpected(artifact_error("body.tasks", "item-length"));
			const auto task_item_end = body.offset() + *task_item_length;
			auto task_tag = body.read_byte("body.tasks");
			auto task_count = body.read_length("body.tasks");
			if (!task_tag || !task_count || *task_tag != 0x05U || *task_count == 0U ||
				*task_count > limits.max_tasks)
				return sdk::unexpected(artifact_error("body.tasks", "tuple-shape"));
			if (*task_count > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(artifact_error("body.tasks", "count-size"));
			auto capture_storage =
				detailed_task_report_replayable_spool::create(replay_capture_limits(limits));
			if (!capture_storage)
				return sdk::unexpected(artifact_error("body.tasks", "spool-create"));
			std::vector<materialization_prior_artifact_task_metadata> tasks;
			tasks.reserve(static_cast<std::size_t>(*task_count));
			std::set<std::string, std::less<>> partitions;
			std::size_t previous_ordinal{};
			bool first{true};
			for (std::size_t index{}; index < static_cast<std::size_t>(*task_count); ++index)
			{
				auto encoded_task = body.read_item("body.tasks.item", limits.max_bytes);
				if (!encoded_task)
					return sdk::unexpected(std::move(encoded_task.error()));
				auto decoded_task_value = sdk::canonical_binary_decode(*encoded_task);
				if (!decoded_task_value)
					return sdk::unexpected(artifact_error("body.tasks.item", "canonical"));
				auto decoded_task = decode_task(*decoded_task_value, limits);
				if (!decoded_task)
					return sdk::unexpected(std::move(decoded_task.error()));
				if ((first && decoded_task->identity.canonical_task_ordinal != 0U) ||
					(!first &&
					 (decoded_task->identity.canonical_task_ordinal <= previous_ordinal ||
					  decoded_task->identity.canonical_task_ordinal != previous_ordinal + 1U)) ||
					!partitions.insert(decoded_task->state.partition_id).second ||
					!sdk::validate_strong_id(decoded_task->identity.provider_task_id) ||
					!sdk::validate_strong_id(decoded_task->identity.task_input_digest) ||
					!sdk::validate_strong_id(
						decoded_task->identity.selected_catalog_compile_unit_id) ||
					!sdk::validate_strong_id(
						decoded_task->identity.final_relation_compile_unit_id) ||
					decoded_task->capture.provider_execution_id.empty())
					return sdk::unexpected(artifact_error("body.tasks", "identity-or-order"));
				if (auto valid =
						validate_prior_artifact_capture_runtime_receipt(decoded_task->capture);
					!valid)
					return sdk::unexpected(std::move(valid.error()));
				previous_ordinal = decoded_task->identity.canonical_task_ordinal;
				first = false;
				tasks.push_back(materialization_prior_artifact_task_metadata{
					decoded_task->identity,
					decoded_task->state,
					decoded_task->sealed_artifact_digest,
					decoded_task->capture.provider_execution_id});
				if (auto appended = capture_storage->append(std::move(decoded_task->capture));
					!appended)
					return sdk::unexpected(artifact_error("body.tasks", "capture-spool"));
			}
			if (body.offset() != task_item_end || body.offset() != body.end())
				return sdk::unexpected(artifact_error("body", "trailing-bytes"));
			if (auto sealed = capture_storage->seal(); !sealed)
				return sdk::unexpected(artifact_error("body.tasks", "spool-seal"));

			materialization_prior_artifact_replay_bundle output;
			output.publication.schema = std::move(*body_schema);
			output.publication.version = static_cast<std::uint32_t>(*body_version);
			output.publication.selector = std::move(*selector);
			output.publication.series_id = std::move(std::get<1>(*publication));
			output.publication.publication_id = std::move(std::get<0>(*publication));
			output.publication.snapshot_id = std::move(std::get<2>(*publication));
			output.publication.sequence = std::get<3>(*publication);
			output.publication.physical_generation = std::get<4>(*publication);
			output.publication.parent_publication = std::move(std::get<5>(*publication));
			output.publication.publication_state = std::get<6>(*publication);
			output.publication.publication_corrupt = std::get<7>(*publication);
			if (output.publication.publication_state != sdk::publication_state::committed ||
				output.publication.publication_corrupt ||
				output.publication.series_id != output.publication.selector.id() ||
				!sdk::validate_strong_id(output.publication.series_id) ||
				!sdk::validate_strong_id(output.publication.publication_id) ||
				!sdk::validate_strong_id(output.publication.snapshot_id) ||
				(output.publication.parent_publication &&
				 !sdk::validate_strong_id(*output.publication.parent_publication)))
				return sdk::unexpected(artifact_error("body.publication", "binding"));
			output.tasks = std::move(tasks);
			output.captures = std::move(*capture_storage);
			return output;
		}

		[[nodiscard]] sdk::result<std::unique_ptr<materialization_replayable_spool>>
		encode_spooled_materialization_prior_artifact(
			const materialization_prior_artifact_bundle& publication,
			const detailed_task_report_replayable_spool& captures,
			const std::vector<materialization_prior_artifact_task_metadata>& tasks,
			const materialization_prior_artifact_limits& limits)
		{
			if (!valid_artifact_limits(limits) || tasks.empty() ||
				tasks.size() > limits.max_tasks || publication.schema != artifact_schema ||
				publication.version != artifact_version)
				return sdk::unexpected(artifact_error("bundle", "shape"));
			if (!captures.sealed() || captures.task_count() != tasks.size())
				return sdk::unexpected(artifact_error("task.capture", "spool-lifecycle"));

			std::set<std::string, std::less<>> partitions;
			std::size_t previous_ordinal{};
			bool first{true};
			for (const auto& task : tasks)
			{
				if (auto valid =
						validate_prior_artifact_task_metadata(task, previous_ordinal, first);
					!valid || !partitions.insert(task.state.partition_id).second)
					return sdk::unexpected(artifact_error("bundle.tasks", "identity-or-order"));
				previous_ordinal = task.identity.canonical_task_ordinal;
				first = false;
			}

			try
			{
				auto task_storage = make_materialization_private_spool();
				if (!task_storage)
					return sdk::unexpected(artifact_error("task", "spool-create"));
				auto task_spool = std::move(*task_storage);
				std::vector<std::byte> task_header;
				task_header.push_back(std::byte{0x05});
				append_big_endian_length(task_header, tasks.size());
				if (auto appended = append_spool_bytes(*task_spool, task_header, "task"); !appended)
					return sdk::unexpected(std::move(appended.error()));
				std::size_t capture_index{};
				std::size_t total_capture_bytes{};
				auto replayed = captures.replay(
					[&](detailed_task_report_capture&& capture) -> sdk::result<void>
					{
						if (capture_index >= tasks.size())
							return sdk::unexpected(artifact_error("task.capture", "count"));
						const auto& metadata = tasks[capture_index];
						if (capture.provider_task_id != metadata.identity.provider_task_id ||
							capture.provider_execution_id != metadata.provider_execution_id ||
							capture.task_input_digest != metadata.identity.task_input_digest ||
							capture.selected_catalog_compile_unit_id !=
								metadata.identity.selected_catalog_compile_unit_id ||
							capture.compile_unit_id !=
								metadata.identity.final_relation_compile_unit_id)
							return sdk::unexpected(artifact_error("task.capture", "identity"));
						if (auto valid = validate_prior_artifact_capture_runtime_receipt(capture);
							!valid)
							return valid;
						auto encoded_capture =
							encode_detailed_task_report_capture(capture, capture_limits(limits));
						if (!encoded_capture ||
							encoded_capture->size() > limits.max_capture_bytes ||
							encoded_capture->size() > limits.max_total_capture_bytes ||
							total_capture_bytes >
								limits.max_total_capture_bytes - encoded_capture->size())
							return sdk::unexpected(
								artifact_error("task.capture", "aggregate-limit"));
						total_capture_bytes += encoded_capture->size();
						materialization_prior_artifact_task task{metadata.identity,
																 metadata.state,
																 metadata.sealed_artifact_digest,
																 std::move(capture)};
						auto value = task_value(task, limits);
						if (!value)
							return sdk::unexpected(std::move(value.error()));
						auto encoded_task = sdk::canonical_binary(*value);
						if (!encoded_task)
							return sdk::unexpected(artifact_error("task", "canonical"));
						std::vector<std::byte> task_field;
						append_big_endian_length(task_field, encoded_task->size());
						if (auto appended = append_spool_bytes(*task_spool, task_field, "task");
							!appended)
							return sdk::unexpected(std::move(appended.error()));
						if (auto appended = append_spool_bytes(*task_spool, *encoded_task, "task");
							!appended)
							return sdk::unexpected(std::move(appended.error()));
						++capture_index;
						return {};
					});
				if (!replayed)
					return sdk::unexpected(std::move(replayed.error()));
				if (capture_index != tasks.size())
					return sdk::unexpected(artifact_error("task.capture", "count"));
				if (auto sealed = seal_artifact_spool(*task_spool, "task"); !sealed)
					return sdk::unexpected(std::move(sealed.error()));
				if (task_spool->size_bytes() > limits.max_bytes)
					return sdk::unexpected(artifact_error("envelope", "size-or-encode"));

				auto body_storage = make_materialization_private_spool();
				if (!body_storage)
					return sdk::unexpected(artifact_error("body", "spool-create"));
				auto body_spool = std::move(*body_storage);
				std::vector<std::byte> body_header;
				body_header.push_back(std::byte{0x05});
				append_big_endian_length(body_header, 5U);
				if (auto appended =
						append_canonical_field(body_header, string_value(publication.schema));
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				if (auto appended =
						append_canonical_field(body_header,
											   sdk::canonical_value::from_integer(
												   static_cast<std::int64_t>(publication.version)));
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				if (auto appended =
						append_canonical_field(body_header, selector_value(publication.selector));
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				auto publication_value = make_publication_value(publication);
				if (!publication_value)
					return sdk::unexpected(std::move(publication_value.error()));
				if (auto appended = append_canonical_field(body_header, *publication_value);
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				if (task_spool->size_bytes() > std::numeric_limits<std::size_t>::max() - 8U ||
					body_header.size() > limits.max_bytes ||
					task_spool->size_bytes() > limits.max_bytes - (body_header.size() + 8U))
					return sdk::unexpected(artifact_error("envelope", "size-or-encode"));
				std::vector<std::byte> task_field_length;
				append_big_endian_length(task_field_length, task_spool->size_bytes());
				if (auto appended = append_spool_bytes(*body_spool, body_header, "body"); !appended)
					return sdk::unexpected(std::move(appended.error()));
				if (auto appended = append_spool_bytes(*body_spool, task_field_length, "body");
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				if (auto appended = append_spool_copy(*body_spool, *task_spool, "body"); !appended)
					return sdk::unexpected(std::move(appended.error()));
				// The task tuple is sealed and fully copied into the body spool. Release its
				// descriptor before assembling the envelope so production residency is bounded
				// by the currently assembled spool plus one replay chunk.
				task_spool.reset();
				if (body_spool->size_bytes() > limits.max_bytes)
					return sdk::unexpected(artifact_error("envelope", "size-or-encode"));
				if (auto sealed = seal_artifact_spool(*body_spool, "body"); !sealed)
					return sdk::unexpected(std::move(sealed.error()));
				auto digest = digest_materialization_spool(*body_spool);
				if (!digest)
					return sdk::unexpected(artifact_error("body", "digest"));

				std::vector<std::byte> envelope_prefix;
				envelope_prefix.push_back(std::byte{0x05});
				append_big_endian_length(envelope_prefix, 3U);
				if (auto appended =
						append_canonical_field(envelope_prefix, string_value(artifact_schema));
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				if (body_spool->size_bytes() > std::numeric_limits<std::uint64_t>::max() - 9U)
					return sdk::unexpected(artifact_error("envelope", "size-or-encode"));
				append_big_endian_length(envelope_prefix, body_spool->size_bytes() + 9U);
				envelope_prefix.push_back(std::byte{0x03});
				append_big_endian_length(envelope_prefix, body_spool->size_bytes());
				std::vector<std::byte> envelope_suffix;
				if (auto appended = append_canonical_field(envelope_suffix, string_value(*digest));
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				if (envelope_prefix.size() > limits.max_bytes ||
					body_spool->size_bytes() > limits.max_bytes - envelope_prefix.size() ||
					envelope_suffix.size() >
						limits.max_bytes - envelope_prefix.size() - body_spool->size_bytes())
					return sdk::unexpected(artifact_error("envelope", "size-or-encode"));
				auto envelope_storage = make_materialization_private_spool();
				if (!envelope_storage)
					return sdk::unexpected(artifact_error("envelope", "spool-create"));
				auto envelope_spool = std::move(*envelope_storage);
				if (auto appended =
						append_spool_bytes(*envelope_spool, envelope_prefix, "envelope");
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				if (auto appended = append_spool_copy(*envelope_spool, *body_spool, "envelope");
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				if (auto appended =
						append_spool_bytes(*envelope_spool, envelope_suffix, "envelope");
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				if (auto sealed = seal_artifact_spool(*envelope_spool, "envelope"); !sealed)
					return sdk::unexpected(std::move(sealed.error()));
				return envelope_spool;
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(artifact_error("envelope", "allocation"));
			}
		}

		[[nodiscard]] sdk::result<void>
		validate_prior_artifact_publication(const validated_publication_request& publication,
											const sdk::publication_record& committed_record,
											const materialization_store_observation& observation)
		{
			if ((publication.backend != "memory" && publication.backend != "sqlite") ||
				(publication.backend == "sqlite" && !publication.sqlite_path) ||
				(publication.backend == "memory" &&
				 (!publication.genesis || publication.expected_parent_publication ||
				  publication.sqlite_path)) ||
				observation.backend != publication.backend ||
				observation.selector != publication.selector ||
				observation.series_id != publication.series_id ||
				observation.expected_parent_publication !=
					publication.expected_parent_publication ||
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
				observation.candidate_identity->parent_publication !=
					committed_record.parent_publication ||
				committed_record.state != sdk::publication_state::committed ||
				committed_record.corrupt || committed_record.series_id != publication.series_id ||
				committed_record.publication_id.empty() ||
				committed_record.parent_publication != publication.expected_parent_publication)
				return sdk::unexpected(artifact_error("sidecar", "publication"));
			return {};
		}

	} // namespace

	bool materialization_prior_artifact_task::operator==(
		const materialization_prior_artifact_task& other) const
	{
		if (identity != other.identity || state != other.state ||
			sealed_artifact_digest != other.sealed_artifact_digest)
			return false;

		return same_capture(capture, other.capture);
	}

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

	sdk::result<std::optional<materialization_prior_artifact_replay_bundle>>
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
			// The installed process is one-shot and the memory Store has no durable artifact
			// namespace.  Keep this branch as an explicit cold-start result instead of exposing a
			// process-local cache that could be mistaken for cross-invocation reuse.
			return std::optional<materialization_prior_artifact_replay_bundle>{};
		}
		if (publication.backend != "sqlite" || publication.genesis ||
			!publication.expected_parent_publication || !publication.sqlite_path)
			return std::optional<materialization_prior_artifact_replay_bundle>{};
		auto path =
			sidecar_path(*publication.sqlite_path, *publication.expected_parent_publication);
		if (!path)
			return sdk::unexpected(std::move(path.error()));
		auto opened = root.open_beneath(*path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (!opened)
		{
			if (is_missing_sidecar(opened.error()))
				return std::optional<materialization_prior_artifact_replay_bundle>{};
			return sdk::unexpected(std::move(opened.error()));
		}
		auto sidecar = spool_sidecar(*opened, limits);
		if (!sidecar)
			return sdk::unexpected(std::move(sidecar.error()));
		auto decoded = decode_streamed_prior_artifact(**sidecar, limits);
		if (!decoded)
			return sdk::unexpected(std::move(decoded.error()));
		if (decoded->publication.publication_id != *publication.expected_parent_publication ||
			decoded->publication.series_id != publication.series_id ||
			decoded->publication.selector != publication.selector)
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
		auto snapshot = store->open(decoded->publication.snapshot_id);
		if (!snapshot)
			return sdk::unexpected(artifact_error("sidecar", "snapshot-not-present"));
		const auto same_record = [&](const sdk::publication_record& record)
		{
			return record.publication_id == decoded->publication.publication_id &&
				record.series_id == decoded->publication.series_id &&
				record.snapshot_id == decoded->publication.snapshot_id &&
				record.sequence == decoded->publication.sequence &&
				record.physical_generation == decoded->publication.physical_generation &&
				record.parent_publication == decoded->publication.parent_publication &&
				record.state == decoded->publication.publication_state &&
				record.corrupt == decoded->publication.publication_corrupt;
		};
		if (!same_record(expected_record->publication()) || !same_record(current->publication()) ||
			!same_record(snapshot->publication()) ||
			expected_record->manifest() != current->manifest() ||
			expected_record->manifest() != snapshot->manifest())
			return sdk::unexpected(artifact_error("sidecar", "store-identity"));
		return std::optional<materialization_prior_artifact_replay_bundle>{std::move(*decoded)};
	}

	sdk::result<void> persist_materialization_prior_artifact(
		const materialization_effect_root& root,
		const validated_publication_request& publication,
		const sdk::publication_record& committed_record,
		const materialization_store_observation& observation,
		const detailed_task_report_replayable_spool& captures,
		std::vector<materialization_prior_artifact_task_metadata> tasks,
		const materialization_prior_artifact_limits& limits)
	{
		if (auto valid =
				validate_prior_artifact_publication(publication, committed_record, observation);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		if (publication.backend == "memory")
			return sdk::unexpected(artifact_error("memory", "process-lifetime-only"));
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
		auto encoded =
			encode_spooled_materialization_prior_artifact(bundle, captures, tasks, limits);
		if (!encoded)
			return sdk::unexpected(std::move(encoded.error()));
		auto path = sidecar_path(*publication.sqlite_path, committed_record.publication_id);
		if (!path)
			return sdk::unexpected(std::move(path.error()));
		return install_sidecar_spool(root, *path, **encoded, limits);
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
		const auto context_mismatch = [&]() -> std::string_view
		{
			if (artifact.capture.project_id != input.project)
				return "project";
			if (artifact.capture.catalog_id != input.project_catalog.catalog_id)
				return "catalog-id";
			if (artifact.capture.catalog_digest != input.project_catalog.catalog_digest)
				return "catalog-digest";
			if (artifact.capture.selected_catalog_compile_unit_id !=
				input.selected_catalog_compile_unit)
				return "selected-catalog-compile-unit";
			if (artifact.capture.compile_unit_id != input.compile_unit)
				return "compile-unit";
			if (artifact.capture.variant_id != input.variant)
				return "variant";
			if (artifact.capture.toolchain_context_id != input.toolchain_context)
				return "toolchain-context";
			if (artifact.capture.toolchain_digest != input.toolchain_digest)
				return "toolchain-digest";
			if (artifact.capture.source_snapshot_id != input.source_snapshot)
				return "source-snapshot";
			if (artifact.capture.source_file_id != input.file)
				return "source-file";
			if (artifact.capture.source_logical_path != input.logical_path)
				return "source-logical-path";
			if (artifact.capture.source_content_digest != input.source_content_digest)
				return "source-content-digest";
			if (artifact.capture.source_size_bytes != input.source_size_bytes)
				return "source-size";
			if (artifact.capture.source_encoding != input.source_encoding)
				return "source-encoding";
			if (artifact.capture.source_line_index_id != input.line_index)
				return "source-line-index";
			if (artifact.capture.source_read_only != input.source_read_only)
				return "source-read-only";
			if (artifact.capture.condition_universe_id != input.condition_universe)
				return "condition-universe";
			if (artifact.capture.condition_id != input.condition)
				return "condition";
			if (artifact.capture.interpretation_domain != input.interpretation)
				return "interpretation";
			return {};
		}();
		if (!context_mismatch.empty())
			return sdk::unexpected(
				artifact_error("task.capture", "current-context/" + std::string{context_mismatch}));
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
		auto frames =
			sdk::provider::decode_frame_stream(artifact.capture.raw_frame_stream, protocol_limits);
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
		if (!raw_validation ||
			raw_validation->kind != sdk::provider::detail::transcript_terminal_kind::complete ||
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
			if (!same_sealed_provider_transcript(*raw_validation->sealed(),
												 result->provider_seal()))
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
