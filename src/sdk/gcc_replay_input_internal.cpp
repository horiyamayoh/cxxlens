#include "gcc_replay_input_internal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::size_t maximum_requested_relations{4096U};
		constexpr std::size_t maximum_unresolved{10000U};

		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {
				"application-analysis.replay-input-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error limit(std::string field, std::string detail)
		{
			return {
				"application-analysis.import-limit-exceeded", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool digest_like(const std::string_view value)
		{
			const auto marker = value.rfind("sha256:");
			return marker != std::string_view::npos && marker + 7U + 64U == value.size() &&
				std::ranges::all_of(value.substr(marker + 7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool source_role(const std::string_view value) noexcept
		{
			return value == "main" || value == "header" || value == "generated" ||
				value == "forced-include" || value == "macro-file";
		}

		[[nodiscard]] bool logical_path(const std::string_view value) noexcept
		{
			if (!value.starts_with("project://") || value.size() == 10U)
				return false;
			std::size_t offset{10U};
			while (offset < value.size())
			{
				const auto next = value.find('/', offset);
				const auto segment = value.substr(
					offset, next == std::string_view::npos ? value.size() - offset : next - offset);
				if (segment.empty() || segment == "." || segment == "..")
					return false;
				if (next == std::string_view::npos)
					break;
				offset = next + 1U;
			}
			return true;
		}

		[[nodiscard]] result<void> text(const std::string_view value,
										const std::string& field,
										const import_limits& limits,
										std::size_t& metadata_bytes)
		{
			if (value.empty() || value.size() > limits.maximum_string_bytes)
				return unexpected(limit(field, "string-bytes"));
			if (value.size() > limits.maximum_total_metadata_bytes - metadata_bytes)
				return unexpected(limit("metadata", "total-bytes"));
			if (auto valid = validate_utf8_text(value); !valid)
				return unexpected(invalid(field, "utf8"));
			metadata_bytes += value.size();
			return {};
		}

		[[nodiscard]] result<std::uint64_t> read_length(const std::span<const std::byte> input,
														std::size_t& offset)
		{
			if (offset > input.size() || input.size() - offset < 8U)
				return unexpected(invalid("binary", "truncated-length"));
			std::uint64_t value{};
			for (std::size_t index{}; index < 8U; ++index)
				value = (value << 8U) | std::to_integer<unsigned char>(input[offset + index]);
			offset += 8U;
			return value;
		}

		[[nodiscard]] result<void> preflight(const std::span<const std::byte> input,
											 const std::size_t depth,
											 const import_limits& limits)
		{
			if (depth > limits.maximum_nesting_depth)
				return unexpected(limit("binary", "nesting-depth"));
			if (input.empty())
				return unexpected(invalid("binary", "missing-tag"));
			std::size_t offset{1U};
			switch (std::to_integer<unsigned char>(input.front()))
			{
				case 0x00U:
					break;
				case 0x01U:
					if (input.size() - offset != 1U)
						return unexpected(invalid("binary", "boolean-size"));
					++offset;
					break;
				case 0x02U:
				{
					if (offset == input.size())
						return unexpected(invalid("binary", "integer-sign"));
					++offset;
					auto width = read_length(input, offset);
					if (!width || *width > input.size() - offset)
						return unexpected(width ? invalid("binary", "integer-width")
												: std::move(width.error()));
					offset += static_cast<std::size_t>(*width);
					break;
				}
				case 0x03U:
				case 0x04U:
				{
					auto size = read_length(input, offset);
					if (!size || *size > input.size() - offset)
						return unexpected(size ? invalid("binary", "payload-size")
											   : std::move(size.error()));
					offset += static_cast<std::size_t>(*size);
					break;
				}
				case 0x05U:
				{
					auto count = read_length(input, offset);
					if (!count || *count > (input.size() - offset) / 9U)
						return unexpected(count ? invalid("binary", "tuple-count")
												: std::move(count.error()));
					for (std::uint64_t index{}; index < *count; ++index)
					{
						auto size = read_length(input, offset);
						if (!size || *size == 0U || *size > input.size() - offset)
							return unexpected(size ? invalid("binary", "tuple-item-size")
												   : std::move(size.error()));
						if (auto valid =
								preflight(input.subspan(offset, static_cast<std::size_t>(*size)),
										  depth + 1U,
										  limits);
							!valid)
							return valid;
						offset += static_cast<std::size_t>(*size);
					}
					break;
				}
				default:
					return unexpected(invalid("binary", "unknown-tag"));
			}
			if (offset != input.size())
				return unexpected(invalid("binary", "trailing-bytes"));
			return {};
		}

		[[nodiscard]] result<const std::vector<canonical_value>*>
		tuple(const canonical_value& value, const std::string& field, const std::size_t count)
		{
			if (value.type != canonical_value::kind::ordered_tuple || value.tuple.size() != count)
				return unexpected(invalid(field, "tuple-shape"));
			return &value.tuple;
		}

		[[nodiscard]] result<std::string> string(const canonical_value& value,
												 const std::string& field)
		{
			if (value.type != canonical_value::kind::utf8_string)
				return unexpected(invalid(field, "string"));
			return value.text;
		}

		[[nodiscard]] canonical_value member_value(const decoded_capture_source_member& value)
		{
			return canonical_value::from_tuple({
				canonical_value::from_string(value.logical_path),
				canonical_value::from_string(value.content_digest),
				canonical_value::from_bytes(value.content),
				canonical_value::from_string(value.role),
			});
		}

		[[nodiscard]] canonical_value gap_value(const capture_gap& value)
		{
			return canonical_value::from_tuple({
				canonical_value::from_string(value.field),
				canonical_value::from_string(value.state),
				canonical_value::from_string(value.reason),
				canonical_value::from_string(value.completion_action),
			});
		}

		[[nodiscard]] bool gap_less(const capture_gap& left, const capture_gap& right)
		{
			return std::tie(left.field, left.state, left.reason, left.completion_action) <
				std::tie(right.field, right.state, right.reason, right.completion_action);
		}

		[[nodiscard]] result<std::string>
		recompute_plan_digest(const replay_plan::implementation& plan)
		{
			std::vector<canonical_value> effective;
			effective.reserve(plan.effective_arguments.size());
			for (const auto& argument : plan.effective_arguments)
				effective.push_back(canonical_value::from_string(argument));
			std::vector<canonical_value> mappings;
			mappings.reserve(plan.option_mappings.size());
			for (const auto& mapping : plan.option_mappings)
			{
				static constexpr std::array fidelity_names{
					std::string_view{"exact"},
					std::string_view{"semantics_preserving"},
					std::string_view{"approximation"},
					std::string_view{"unsupported"},
					std::string_view{"nonsemantic"},
				};
				if (!is_valid(mapping.fidelity))
					return unexpected(invalid("replay_plan.option_mappings", "fidelity"));
				std::vector<canonical_value> replay_tokens;
				for (const auto& token : mapping.replay_tokens)
					replay_tokens.push_back(canonical_value::from_string(token));
				mappings.push_back(canonical_value::from_tuple({
					canonical_value::from_string(mapping.production_token),
					canonical_value::from_tuple(std::move(replay_tokens)),
					canonical_value::from_string(
						std::string{fidelity_names[static_cast<std::size_t>(mapping.fidelity)]}),
					canonical_value::from_string(mapping.affected_scope),
					canonical_value::from_string(mapping.reason),
					canonical_value::from_string(mapping.completion_action),
				}));
			}
			std::vector<canonical_value> unresolved;
			for (const auto& gap : plan.unresolved)
				unresolved.push_back(gap_value(gap));
			auto encoded = canonical_binary(canonical_value::from_tuple({
				canonical_value::from_string("cxxlens.compiler-replay-plan.v1"),
				canonical_value::from_string(plan.capture_bundle_digest),
				canonical_value::from_string(plan.compile_unit_id),
				canonical_value::from_string(plan.analysis_frontend),
				canonical_value::from_string(plan.target_abi),
				canonical_value::from_tuple(std::move(effective)),
				canonical_value::from_tuple(std::move(mappings)),
				canonical_value::from_string(plan.source_closure_digest),
				canonical_value::from_tuple(std::move(unresolved)),
			}));
			if (!encoded)
				return unexpected(invalid("replay_plan", "canonical-encoding"));
			return content_digest(*encoded);
		}
	} // namespace

	result<validated_gcc_replay_input> validate_gcc_replay_input(gcc_replay_input_draft draft,
																 const import_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return unexpected(std::move(valid.error()));
			std::size_t metadata_bytes{};
			for (const auto& [field, value] : {
					 std::pair{std::string{"imported_project_id"}, &draft.imported_project_id},
					 std::pair{std::string{"capture_bundle_digest"}, &draft.capture_bundle_digest},
					 std::pair{std::string{"replay_plan_digest"}, &draft.replay_plan_digest},
					 std::pair{std::string{"compile_unit_id"}, &draft.compile_unit_id},
					 std::pair{std::string{"analysis_frontend"}, &draft.analysis_frontend},
					 std::pair{std::string{"target_abi"}, &draft.target_abi},
					 std::pair{std::string{"source_closure_digest"}, &draft.source_closure_digest},
					 std::pair{std::string{"interpretation"}, &draft.interpretation},
				 })
				if (auto valid = text(*value, field, limits, metadata_bytes); !valid)
					return unexpected(std::move(valid.error()));
			if (auto valid = validate_strong_id(draft.imported_project_id); !valid)
				return unexpected(invalid("imported_project_id", "strong-id"));
			if (auto valid = validate_strong_id(draft.compile_unit_id); !valid)
				return unexpected(invalid("compile_unit_id", "strong-id"));
			if (auto valid = validate_strong_id(draft.interpretation); !valid)
				return unexpected(invalid("interpretation", "strong-id"));
			if (!digest_like(draft.capture_bundle_digest) ||
				!digest_like(draft.replay_plan_digest) || !digest_like(draft.source_closure_digest))
				return unexpected(invalid("digest", "spelling"));
			if (draft.analysis_frontend != "clang-23.1.0-gcc-mode" ||
				draft.target_abi != "x86_64-linux-gnu")
				return unexpected(invalid("frontend", "unsupported"));
			if (draft.effective_arguments.size() < 2U ||
				draft.effective_arguments.size() > limits.maximum_arguments_per_unit ||
				draft.effective_arguments[0] != "clang++" ||
				draft.effective_arguments[1] != "-fsyntax-only")
				return unexpected(invalid("effective_argv", "shape"));
			for (std::size_t index{}; index < draft.effective_arguments.size(); ++index)
				if (auto valid = text(draft.effective_arguments[index],
									  "effective_argv[" + std::to_string(index) + "]",
									  limits,
									  metadata_bytes);
					!valid)
					return unexpected(std::move(valid.error()));
			if (draft.source_members.empty() ||
				draft.source_members.size() > limits.maximum_source_closure_members)
				return unexpected(limit("source_members", "count"));
			std::ranges::sort(
				draft.source_members, {}, &decoded_capture_source_member::logical_path);
			std::uint64_t source_bytes{};
			std::string previous_path;
			std::size_t main_count{};
			for (std::size_t index{}; index < draft.source_members.size(); ++index)
			{
				auto& member = draft.source_members[index];
				const auto prefix = "source_members[" + std::to_string(index) + "]";
				if (!logical_path(member.logical_path) ||
					(!previous_path.empty() && previous_path >= member.logical_path))
					return unexpected(invalid(prefix + ".logical_path", "canonical-unique"));
				previous_path = member.logical_path;
				if (!digest_like(member.content_digest) ||
					content_digest(member.content) != member.content_digest)
					return unexpected(invalid(prefix + ".content", "digest-mismatch"));
				if (!source_role(member.role))
					return unexpected(invalid(prefix + ".role", "enum"));
				main_count += member.role == "main" ? 1U : 0U;
				if (member.content.size() > limits.maximum_source_closure_bytes - source_bytes)
					return unexpected(limit("source_members", "bytes"));
				source_bytes += member.content.size();
				for (const auto& [field, value] : {
						 std::pair{prefix + ".logical_path", &member.logical_path},
						 std::pair{prefix + ".content_digest", &member.content_digest},
						 std::pair{prefix + ".role", &member.role},
					 })
					if (auto valid = text(*value, field, limits, metadata_bytes); !valid)
						return unexpected(std::move(valid.error()));
			}
			if (main_count != 1U)
				return unexpected(invalid("source_members", "exactly-one-main"));
			const auto main = std::ranges::find(draft.source_members,
												std::string_view{"main"},
												&decoded_capture_source_member::role);
			if (std::ranges::count(draft.effective_arguments, main->logical_path) != 1)
				return unexpected(invalid("effective_argv", "main-source-binding"));
			if (draft.requested_relation_descriptor_ids.empty() ||
				draft.requested_relation_descriptor_ids.size() > maximum_requested_relations)
				return unexpected(limit("requested_relations", "count"));
			std::ranges::sort(draft.requested_relation_descriptor_ids);
			if (std::ranges::adjacent_find(draft.requested_relation_descriptor_ids) !=
				draft.requested_relation_descriptor_ids.end())
				return unexpected(invalid("requested_relations", "duplicate"));
			for (std::size_t index{}; index < draft.requested_relation_descriptor_ids.size();
				 ++index)
			{
				const auto& descriptor = draft.requested_relation_descriptor_ids[index];
				if (auto valid = validate_strong_id(descriptor); !valid)
					return unexpected(invalid("requested_relations", "strong-id"));
				if (auto valid = text(descriptor,
									  "requested_relations[" + std::to_string(index) + "]",
									  limits,
									  metadata_bytes);
					!valid)
					return unexpected(std::move(valid.error()));
			}
			if (draft.unresolved.size() > maximum_unresolved)
				return unexpected(limit("unresolved", "count"));
			std::ranges::sort(draft.unresolved, gap_less);
			draft.unresolved.erase(std::ranges::unique(draft.unresolved).begin(),
								   draft.unresolved.end());
			for (std::size_t index{}; index < draft.unresolved.size(); ++index)
				for (const auto& [field, value] : {
						 std::pair{std::string{"field"}, &draft.unresolved[index].field},
						 std::pair{std::string{"state"}, &draft.unresolved[index].state},
						 std::pair{std::string{"reason"}, &draft.unresolved[index].reason},
						 std::pair{std::string{"completion_action"},
								   &draft.unresolved[index].completion_action},
					 })
					if (auto valid = text(*value,
										  "unresolved[" + std::to_string(index) + "]." + field,
										  limits,
										  metadata_bytes);
						!valid)
						return unexpected(std::move(valid.error()));

			std::vector<canonical_value> arguments;
			for (const auto& value : draft.effective_arguments)
				arguments.push_back(canonical_value::from_string(value));
			std::vector<canonical_value> members;
			for (const auto& value : draft.source_members)
				members.push_back(member_value(value));
			std::vector<canonical_value> relations;
			for (const auto& value : draft.requested_relation_descriptor_ids)
				relations.push_back(canonical_value::from_string(value));
			std::vector<canonical_value> unresolved;
			for (const auto& value : draft.unresolved)
				unresolved.push_back(gap_value(value));
			auto encoded = canonical_binary(canonical_value::from_tuple({
				canonical_value::from_string("cxxlens.gcc-replay-input.v1"),
				canonical_value::from_string(draft.imported_project_id),
				canonical_value::from_string(draft.capture_bundle_digest),
				canonical_value::from_string(draft.replay_plan_digest),
				canonical_value::from_string(draft.compile_unit_id),
				canonical_value::from_string(draft.analysis_frontend),
				canonical_value::from_string(draft.target_abi),
				canonical_value::from_tuple(std::move(arguments)),
				canonical_value::from_string(draft.source_closure_digest),
				canonical_value::from_tuple(std::move(members)),
				canonical_value::from_tuple(std::move(relations)),
				canonical_value::from_string(draft.interpretation),
				canonical_value::from_tuple(std::move(unresolved)),
			}));
			if (!encoded || encoded->size() > limits.maximum_bundle_bytes)
				return unexpected(limit("replay_input", "bytes"));
			auto digest = content_digest(*encoded);
			return validated_gcc_replay_input{
				std::move(draft), std::move(*encoded), std::move(digest)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("replay_input", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("replay_input", "allocation-length"));
		}
	}

	result<validated_gcc_replay_input>
	decode_gcc_replay_input(const std::span<const std::byte> bytes, const import_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return unexpected(std::move(valid.error()));
			if (bytes.empty() || bytes.size() > limits.maximum_bundle_bytes)
				return unexpected(limit("replay_input", "bytes"));
			if (auto valid = preflight(bytes, 0U, limits); !valid)
				return unexpected(std::move(valid.error()));
			auto root = canonical_binary_decode(bytes);
			if (!root)
				return unexpected(invalid("binary", root.error().detail));
			auto fields = tuple(*root, "root", 13U);
			if (!fields)
				return unexpected(std::move(fields.error()));
			gcc_replay_input_draft draft;
			auto assign = [&](const std::size_t index,
							  const std::string& field,
							  std::string& destination) -> result<void>
			{
				auto value = string((**fields)[index], field);
				if (!value)
					return unexpected(std::move(value.error()));
				destination = std::move(*value);
				return {};
			};
			std::string schema;
			for (const auto& [index, field, destination] : {
					 std::tuple{0U, std::string{"schema"}, &schema},
					 std::tuple{1U, std::string{"imported_project_id"}, &draft.imported_project_id},
					 std::tuple{
						 2U, std::string{"capture_bundle_digest"}, &draft.capture_bundle_digest},
					 std::tuple{3U, std::string{"replay_plan_digest"}, &draft.replay_plan_digest},
					 std::tuple{4U, std::string{"compile_unit_id"}, &draft.compile_unit_id},
					 std::tuple{5U, std::string{"analysis_frontend"}, &draft.analysis_frontend},
					 std::tuple{6U, std::string{"target_abi"}, &draft.target_abi},
					 std::tuple{
						 8U, std::string{"source_closure_digest"}, &draft.source_closure_digest},
					 std::tuple{11U, std::string{"interpretation"}, &draft.interpretation},
				 })
				if (auto valid = assign(index, field, *destination); !valid)
					return unexpected(std::move(valid.error()));
			if (schema != "cxxlens.gcc-replay-input.v1")
				return unexpected(invalid("schema", "unsupported"));
			auto arguments = tuple((**fields)[7], "effective_argv", (**fields)[7].tuple.size());
			auto members = tuple((**fields)[9], "source_members", (**fields)[9].tuple.size());
			auto relations =
				tuple((**fields)[10], "requested_relations", (**fields)[10].tuple.size());
			auto unresolved = tuple((**fields)[12], "unresolved", (**fields)[12].tuple.size());
			if (!arguments || !members || !relations || !unresolved)
				return unexpected(invalid("root", "collection-shape"));
			for (std::size_t index{}; index < (*arguments)->size(); ++index)
			{
				auto value = string((**arguments)[index], "effective_argv");
				if (!value)
					return unexpected(std::move(value.error()));
				draft.effective_arguments.push_back(std::move(*value));
			}
			for (std::size_t index{}; index < (*members)->size(); ++index)
			{
				auto member = tuple((**members)[index], "source_members", 4U);
				if (!member || (**member)[2].type != canonical_value::kind::bytes)
					return unexpected(invalid("source_members", "member-shape"));
				decoded_capture_source_member value;
				for (const auto& [field_index, field, destination] : {
						 std::tuple{0U, std::string{"logical_path"}, &value.logical_path},
						 std::tuple{1U, std::string{"content_digest"}, &value.content_digest},
						 std::tuple{3U, std::string{"role"}, &value.role},
					 })
				{
					auto decoded = string((**member)[field_index], field);
					if (!decoded)
						return unexpected(std::move(decoded.error()));
					*destination = std::move(*decoded);
				}
				value.content = (**member)[2].byte_string;
				draft.source_members.push_back(std::move(value));
			}
			for (const auto& relation : **relations)
			{
				auto value = string(relation, "requested_relations");
				if (!value)
					return unexpected(std::move(value.error()));
				draft.requested_relation_descriptor_ids.push_back(std::move(*value));
			}
			for (const auto& unresolved_value : **unresolved)
			{
				auto gap = tuple(unresolved_value, "unresolved", 4U);
				if (!gap)
					return unexpected(std::move(gap.error()));
				capture_gap value;
				std::array destinations{
					&value.field, &value.state, &value.reason, &value.completion_action};
				for (std::size_t index{}; index < destinations.size(); ++index)
				{
					auto decoded = string((**gap)[index], "unresolved");
					if (!decoded)
						return unexpected(std::move(decoded.error()));
					*destinations[index] = std::move(*decoded);
				}
				draft.unresolved.push_back(std::move(value));
			}
			auto validated = validate_gcc_replay_input(std::move(draft), limits);
			if (!validated)
				return unexpected(std::move(validated.error()));
			if (!std::ranges::equal(validated->bytes(), bytes))
				return unexpected(invalid("binary", "noncanonical-set-order"));
			return validated;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("replay_input", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("replay_input", "allocation-length"));
		}
	}

	result<validated_gcc_replay_input>
	make_gcc_replay_input(const imported_project::implementation& project,
						  const replay_plan::implementation& plan,
						  const std::span<const std::string> requested_relation_descriptor_ids,
						  const std::string_view interpretation,
						  const import_limits limits)
	{
		if (!project.capture || project.capture_bundle_digest != project.capture->digest ||
			plan.capture_bundle_digest != project.capture_bundle_digest)
			return unexpected(invalid("capture_bundle_digest", "binding-mismatch"));
		auto recomputed = recompute_plan_digest(plan);
		if (!recomputed || *recomputed != plan.digest)
			return unexpected(invalid("replay_plan_digest", "binding-mismatch"));
		const auto unit = std::ranges::find(project.capture->projection.compile_units,
											plan.compile_unit_id,
											&decoded_capture_unit::compile_unit_id);
		if (unit == project.capture->projection.compile_units.end() ||
			unit->source_closure_digest != plan.source_closure_digest)
			return unexpected(invalid("compile_unit_id", "capture-binding-mismatch"));
		const auto closure = std::ranges::find(project.capture->projection.source_closures,
											   unit->source_closure_id,
											   &decoded_capture_source_closure::id);
		if (closure == project.capture->projection.source_closures.end())
			return unexpected(invalid("source_closure_digest", "capture-binding-mismatch"));
		gcc_replay_input_draft draft;
		draft.imported_project_id = project.id;
		draft.capture_bundle_digest = project.capture_bundle_digest;
		draft.replay_plan_digest = plan.digest;
		draft.compile_unit_id = plan.compile_unit_id;
		draft.analysis_frontend = plan.analysis_frontend;
		draft.target_abi = plan.target_abi;
		draft.effective_arguments = plan.effective_arguments;
		draft.source_closure_digest = plan.source_closure_digest;
		draft.source_members = closure->members;
		draft.requested_relation_descriptor_ids.assign(requested_relation_descriptor_ids.begin(),
													   requested_relation_descriptor_ids.end());
		draft.interpretation = interpretation;
		draft.unresolved = plan.unresolved;
		return validate_gcc_replay_input(std::move(draft), limits);
	}
} // namespace cxxlens::sdk::detail
