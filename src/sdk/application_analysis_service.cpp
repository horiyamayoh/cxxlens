#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

#include <cxxlens/sdk/application_analysis.hpp>

#include "application_analysis_internal.hpp"
#include "gcc_auxiliary_capture_internal.hpp"

namespace cxxlens::sdk
{
	struct materialization_request::implementation
	{
		relation_engine engine;
		snapshot_draft publication;
		std::vector<std::string> relation_descriptor_ids;
		std::string interpretation;
		provider::provider_selection_request provider;
		provider::execution_budget budget;
		std::stop_token cancellation;
	};

	struct materialization_result::implementation
	{
		materialization_terminal terminal{materialization_terminal::failed};
		std::optional<snapshot_handle> published_snapshot;
		std::vector<provider::coverage_unit> coverage;
		std::vector<provider::unresolved_item> unresolved;
		std::vector<claim_conflict> conflicts;
		std::vector<differential_disagreement> differential_disagreements;
		std::optional<application_analysis_provenance> provenance;
	};

	namespace
	{
		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.request-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool digest_like(const std::string_view value)
		{
			const auto marker = value.rfind("sha256:");
			if (marker == std::string_view::npos || marker + 7U + 64U != value.size())
				return false;
			return std::ranges::all_of(value.substr(marker + 7U),
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] std::string_view fidelity_name(const replay_fidelity value) noexcept
		{
			switch (value)
			{
				case replay_fidelity::exact:
					return "exact";
				case replay_fidelity::semantics_preserving:
					return "semantics_preserving";
				case replay_fidelity::approximation:
					return "approximation";
				case replay_fidelity::unsupported:
					return "unsupported";
				case replay_fidelity::nonsemantic:
					return "nonsemantic";
			}
			return {};
		}

		[[nodiscard]] bool gap_less(const capture_gap& left, const capture_gap& right)
		{
			return std::tie(left.field, left.state, left.reason, left.completion_action) <
				std::tie(right.field, right.state, right.reason, right.completion_action);
		}

		void canonicalize_gaps(std::vector<capture_gap>& values)
		{
			std::ranges::sort(values, gap_less);
			values.erase(std::ranges::unique(values).begin(), values.end());
		}

		void append_gap(std::vector<capture_gap>& values,
						std::string field,
						std::string reason,
						std::string action)
		{
			values.push_back(
				{std::move(field), "unavailable", std::move(reason), std::move(action)});
		}

		[[nodiscard]] std::optional<std::string>
		logical_path_for(const detail::decoded_capture_projection& capture,
						 std::string_view physical);

		[[nodiscard]] result<std::string>
		logical_auxiliary_path(const detail::decoded_capture_projection& capture,
							   const detail::decoded_capture_unit& unit,
							   const std::string_view value)
		{
			if (value.empty() || value.contains('\0') || value.contains('\\'))
				return unexpected(invalid("response_file.path", "logical-path-required"));
			if (value.starts_with('/'))
			{
				auto mapped = logical_path_for(capture, value);
				if (!mapped)
					return unexpected(invalid("response_file.path", "outside-project-root"));
				return *mapped;
			}
			std::string relative;
			if (value.starts_with("project://"))
				relative = std::string{value.substr(std::string_view{"project://"}.size())};
			else
			{
				if (!unit.logical_working_directory.starts_with("project://"))
					return unexpected(invalid("response_file.path", "working-directory"));
				relative =
					unit.logical_working_directory.substr(std::string_view{"project://"}.size());
				if (!relative.empty())
					relative.push_back('/');
				relative += value;
			}
			std::vector<std::string_view> segments;
			std::size_t offset{};
			while (offset < relative.size())
			{
				const auto next = relative.find('/', offset);
				const auto segment = std::string_view{relative}.substr(
					offset, next == std::string::npos ? relative.size() - offset : next - offset);
				if (segment.empty())
					return unexpected(invalid("response_file.path", "empty-segment"));
				if (segment == "..")
				{
					if (segments.empty())
						return unexpected(invalid("response_file.path", "outside-project-root"));
					segments.pop_back();
				}
				else if (segment != ".")
					segments.push_back(segment);
				if (next == std::string::npos)
					break;
				offset = next + 1U;
			}
			std::string output{"project://"};
			for (std::size_t index{}; index < segments.size(); ++index)
			{
				if (index != 0U)
					output.push_back('/');
				output += segments[index];
			}
			return output;
		}

		class gcc_response_expander
		{
		  public:
			gcc_response_expander(const detail::decoded_capture_projection& capture,
								  const detail::decoded_capture_unit& unit,
								  const detail::decoded_capture_source_closure& closure,
								  const import_limits& limits)
				: capture_{capture}, unit_{unit}, closure_{closure}, limits_{limits}
			{
			}

			[[nodiscard]] result<std::vector<std::string>>
			expand(const std::span<const std::string> arguments, const std::size_t depth)
			{
				std::vector<std::string> output;
				for (const auto& argument : arguments)
				{
					if (!argument.starts_with('@') || argument.size() == 1U)
					{
						if (output.size() >= limits_.maximum_arguments_per_unit)
							return unexpected(error{"application-analysis.import-limit-exceeded",
													"response_file.arguments",
													"count"});
						output.push_back(argument);
						continue;
					}
					if (++expansions_ > 1999U)
						return unexpected(error{"application-analysis.import-limit-exceeded",
												"response_files",
												"gcc-expansion-count"});
					if (depth >= limits_.maximum_nesting_depth)
						return unexpected(error{"application-analysis.import-limit-exceeded",
												"response_files",
												"depth"});
					auto path = logical_auxiliary_path(
						capture_, unit_, std::string_view{argument}.substr(1U));
					if (!path)
						return unexpected(std::move(path.error()));
					const auto metadata =
						std::ranges::find(unit_.response_files,
										  *path,
										  &detail::decoded_capture_auxiliary_file::logical_path);
					if (metadata == unit_.response_files.end() || !metadata->content_digest)
					{
						if (output.size() >= limits_.maximum_arguments_per_unit)
							return unexpected(error{"application-analysis.import-limit-exceeded",
													"response_file.arguments",
													"count"});
						output.push_back(argument);
						continue;
					}
					const auto member =
						std::ranges::find(closure_.members,
										  *path,
										  &detail::decoded_capture_source_member::logical_path);
					if (member == closure_.members.end())
						return unexpected(
							invalid("response_files", "source-closure-binding-mismatch"));
					if (!active_.emplace(*path).second)
						return unexpected(invalid("response_files", "recursive-reference"));
					auto parsed =
						detail::parse_gcc_16_2_response_arguments(member->content, limits_);
					if (!parsed)
						return unexpected(std::move(parsed.error()));
					auto nested = expand(*parsed, depth + 1U);
					active_.erase(*path);
					if (!nested)
						return unexpected(std::move(nested.error()));
					if (nested->size() > limits_.maximum_arguments_per_unit - output.size())
						return unexpected(error{"application-analysis.import-limit-exceeded",
												"response_file.arguments",
												"count"});
					output.insert(output.end(), nested->begin(), nested->end());
				}
				return output;
			}

		  private:
			const detail::decoded_capture_projection& capture_;
			const detail::decoded_capture_unit& unit_;
			const detail::decoded_capture_source_closure& closure_;
			const import_limits& limits_;
			std::set<std::string, std::less<>> active_;
			std::size_t expansions_{};
		};

		[[nodiscard]] result<std::vector<std::string>>
		expand_gcc_response_arguments(const detail::decoded_capture_projection& capture,
									  const detail::decoded_capture_unit& unit,
									  const import_limits& limits)
		{
			const auto closure = std::ranges::find(capture.source_closures,
												   unit.source_closure_id,
												   &detail::decoded_capture_source_closure::id);
			if (closure == capture.source_closures.end())
				return unexpected(invalid("source_closure", "missing-validated-binding"));
			return gcc_response_expander{capture, unit, *closure, limits}.expand(
				*unit.original_arguments, 0U);
		}

		[[nodiscard]] std::vector<capture_gap>
		capture_gaps_for_unit(const std::span<const capture_gap> gaps, const std::size_t unit_index)
		{
			const auto unit_prefix = "compile_units[" + std::to_string(unit_index) + "].";
			std::vector<capture_gap> output;
			for (const auto& gap : gaps)
				if (!gap.field.starts_with("compile_units[") || gap.field.starts_with(unit_prefix))
					output.push_back(gap);
			return output;
		}

		[[nodiscard]] std::optional<std::string>
		logical_path_for(const detail::decoded_capture_projection& capture,
						 const std::string_view physical)
		{
			for (const auto& mapping : capture.path_mappings)
			{
				if (!physical.starts_with(mapping.physical_prefix))
					continue;
				if (physical.size() != mapping.physical_prefix.size() &&
					mapping.physical_prefix.back() != '/' &&
					physical[mapping.physical_prefix.size()] != '/')
					continue;
				auto suffix = physical.substr(mapping.physical_prefix.size());
				if (mapping.logical_prefix.ends_with('/') && suffix.starts_with('/'))
					suffix.remove_prefix(1U);
				return mapping.logical_prefix + std::string{suffix};
			}
			return std::nullopt;
		}

		[[nodiscard]] canonical_value mapping_value(const detail::replay_option_mapping& value)
		{
			std::vector<canonical_value> replay_tokens;
			replay_tokens.reserve(value.replay_tokens.size());
			for (const auto& token : value.replay_tokens)
				replay_tokens.push_back(canonical_value::from_string(token));
			return canonical_value::from_tuple({
				canonical_value::from_string(value.production_token),
				canonical_value::from_tuple(std::move(replay_tokens)),
				canonical_value::from_string(std::string{fidelity_name(value.fidelity)}),
				canonical_value::from_string(value.affected_scope),
				canonical_value::from_string(value.reason),
				canonical_value::from_string(value.completion_action),
			});
		}

		[[nodiscard]] canonical_value gap_value(const capture_gap& value)
		{
			return canonical_value::from_tuple(
				{canonical_value::from_string(value.field),
				 canonical_value::from_string(value.state),
				 canonical_value::from_string(value.reason),
				 canonical_value::from_string(value.completion_action)});
		}

		[[nodiscard]] result<std::shared_ptr<replay_plan::implementation>>
		make_gcc_replay_plan(const capture_bundle::implementation& bundle,
							 const detail::decoded_capture_unit& unit,
							 const std::size_t unit_index,
							 const import_limits& limits)
		{
			if (!unit.original_arguments || unit.original_arguments->empty())
				return unexpected(error{"application-analysis.target-unavailable",
										"original_argv",
										"recapture the GCC compile unit with its exact argv"});
			if (unit.original_arguments->size() > limits.maximum_arguments_per_unit)
				return unexpected(
					error{"application-analysis.import-limit-exceeded", "original_argv", "count"});
			auto expanded_arguments =
				expand_gcc_response_arguments(bundle.projection, unit, limits);
			if (!expanded_arguments)
				return unexpected(std::move(expanded_arguments.error()));

			auto value = std::make_shared<replay_plan::implementation>();
			value->capture_bundle_digest = bundle.digest;
			value->compile_unit_id = unit.compile_unit_id;
			value->analysis_frontend = "clang-23.1.0-gcc-mode";
			value->target_abi = bundle.target_abi;
			value->source_closure_digest = unit.source_closure_digest;
			if (value->source_closure_digest.empty())
				return unexpected(error{"application-analysis.capture-invalid",
										"source_closure",
										"missing-validated-binding"});
			value->unresolved = capture_gaps_for_unit(bundle.gaps, unit_index);
			value->effective_arguments.reserve(expanded_arguments->size() + 1U);
			value->option_mappings.reserve(expanded_arguments->size());

			bool source_bound = false;
			bool output_argument = false;
			for (std::size_t index{}; index < expanded_arguments->size(); ++index)
			{
				const auto& token = (*expanded_arguments)[index];
				if (token.size() > limits.maximum_string_bytes)
					return unexpected(error{"application-analysis.import-limit-exceeded",
											"original_argv",
											"string-bytes"});
				detail::replay_option_mapping mapping;
				mapping.production_token = token;
				const auto field = "compile_units[" + std::to_string(unit_index) +
					"].expanded_argv[" + std::to_string(index) + "]";

				if (index == 0U)
				{
					mapping.replay_tokens = {"clang++", "-fsyntax-only"};
					mapping.fidelity = replay_fidelity::approximation;
					mapping.affected_scope = unit.compile_unit_id;
					mapping.reason = "analysis-frontend-differs-from-production-compiler";
					mapping.completion_action = "use-a-qualified-gcc-native-gap-provider";
					value->effective_arguments.insert(value->effective_arguments.end(),
													  mapping.replay_tokens.begin(),
													  mapping.replay_tokens.end());
					append_gap(value->unresolved, field, mapping.reason, mapping.completion_action);
				}
				else if (output_argument)
				{
					mapping.fidelity = replay_fidelity::nonsemantic;
					output_argument = false;
				}
				else if (token == "-c")
					mapping.fidelity = replay_fidelity::nonsemantic;
				else if (token == "-o")
				{
					mapping.fidelity = replay_fidelity::nonsemantic;
					output_argument = true;
				}
				else if (token.starts_with("-std="))
				{
					const auto standard = token.substr(5U);
					const bool matches =
						unit.language_standard && *unit.language_standard == standard;
					const bool strict = unit.extension_mode && *unit.extension_mode == "strict";
					if (matches && strict && standard == "c++23")
					{
						mapping.replay_tokens = {token};
						mapping.fidelity = replay_fidelity::exact;
						value->effective_arguments.push_back(token);
					}
					else if (matches && unit.extension_mode && *unit.extension_mode == "gnu" &&
							 standard == "gnu++23")
					{
						mapping.replay_tokens = {token};
						mapping.fidelity = replay_fidelity::approximation;
						mapping.affected_scope = unit.compile_unit_id;
						mapping.reason = "gcc-extension-fidelity-not-proved-for-clang-replay";
						mapping.completion_action =
							"compare-the-required-extension-or-use-native-gap-provider";
						value->effective_arguments.push_back(token);
						append_gap(
							value->unresolved, field, mapping.reason, mapping.completion_action);
					}
					else
					{
						mapping.fidelity = replay_fidelity::unsupported;
						mapping.affected_scope = unit.compile_unit_id;
						mapping.reason = "language-mode-capture-mismatch-or-unsupported";
						mapping.completion_action = "recapture-a-pinned-cxx23-language-mode";
						append_gap(
							value->unresolved, field, mapping.reason, mapping.completion_action);
					}
				}
				else
				{
					auto logical = logical_path_for(bundle.projection, token);
					if ((logical && *logical == unit.source_logical_path) ||
						token == unit.source_logical_path)
					{
						const auto replay = logical ? *logical : token;
						mapping.replay_tokens = {replay};
						mapping.fidelity = logical ? replay_fidelity::semantics_preserving
												   : replay_fidelity::exact;
						value->effective_arguments.push_back(replay);
						source_bound = true;
					}
					else
					{
						mapping.fidelity = replay_fidelity::unsupported;
						mapping.affected_scope = unit.compile_unit_id;
						mapping.reason = token.starts_with('@')
							? "response-file-expansion-unavailable"
							: "gcc-option-not-classified";
						mapping.completion_action = token.starts_with('@')
							? "capture-and-expand-the-response-file"
							: "add-a-versioned-gcc16-option-mapping";
						append_gap(
							value->unresolved, field, mapping.reason, mapping.completion_action);
					}
				}
				value->option_mappings.push_back(std::move(mapping));
			}
			if (output_argument)
				return unexpected(error{"application-analysis.capture-invalid",
										"original_argv",
										"missing-output-path"});
			if (!source_bound)
			{
				value->effective_arguments.push_back(unit.source_logical_path);
				append_gap(value->unresolved,
						   "compile_units[" + std::to_string(unit_index) + "].original_argv",
						   "source-token-not-bound",
						   "recapture-the-exact-production-source-token");
			}
			canonicalize_gaps(value->unresolved);

			std::vector<canonical_value> effective;
			effective.reserve(value->effective_arguments.size());
			for (const auto& token : value->effective_arguments)
				effective.push_back(canonical_value::from_string(token));
			std::vector<canonical_value> mappings;
			mappings.reserve(value->option_mappings.size());
			for (const auto& mapping : value->option_mappings)
				mappings.push_back(mapping_value(mapping));
			std::vector<canonical_value> unresolved;
			unresolved.reserve(value->unresolved.size());
			for (const auto& gap : value->unresolved)
				unresolved.push_back(gap_value(gap));
			auto encoded = canonical_binary(canonical_value::from_tuple({
				canonical_value::from_string("cxxlens.compiler-replay-plan.v1"),
				canonical_value::from_string(value->capture_bundle_digest),
				canonical_value::from_string(value->compile_unit_id),
				canonical_value::from_string(value->analysis_frontend),
				canonical_value::from_string(value->target_abi),
				canonical_value::from_tuple(std::move(effective)),
				canonical_value::from_tuple(std::move(mappings)),
				canonical_value::from_string(value->source_closure_digest),
				canonical_value::from_tuple(std::move(unresolved)),
			}));
			if (!encoded)
				return unexpected(
					error{"application-analysis.capture-invalid", "replay_plan", "encoding"});
			value->digest = content_digest(*encoded);
			return value;
		}
	} // namespace

	replay_plan::replay_plan(std::shared_ptr<const implementation> value) : value_{std::move(value)}
	{
	}

	std::string_view replay_plan::digest() const noexcept
	{
		return value_->digest;
	}
	std::string_view replay_plan::capture_bundle_digest() const noexcept
	{
		return value_->capture_bundle_digest;
	}
	std::string_view replay_plan::compile_unit_id() const noexcept
	{
		return value_->compile_unit_id;
	}
	std::string_view replay_plan::analysis_frontend() const noexcept
	{
		return value_->analysis_frontend;
	}
	std::string_view replay_plan::target_abi() const noexcept
	{
		return value_->target_abi;
	}
	std::span<const capture_gap> replay_plan::unresolved() const noexcept
	{
		return value_->unresolved;
	}

	imported_project::imported_project(std::shared_ptr<const implementation> value)
		: value_{std::move(value)}
	{
	}

	std::string_view imported_project::id() const noexcept
	{
		return value_->id;
	}
	std::string_view imported_project::capture_bundle_digest() const noexcept
	{
		return value_->capture_bundle_digest;
	}
	std::span<const replay_plan> imported_project::replay_plans() const noexcept
	{
		return value_->replay_plans;
	}
	std::span<const capture_gap> imported_project::unresolved() const noexcept
	{
		return value_->unresolved;
	}

	result<imported_project> import_capture(const capture_bundle& bundle,
											const import_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (bundle.value_->projection.toolchain_family != "gcc")
			return unexpected(error{"application-analysis.target-unavailable",
									"replay-planner",
									"MSVC replay is not configured"});
		if (bundle.value_->projection.compile_units.size() > limits.maximum_compile_units)
			return unexpected(
				error{"application-analysis.import-limit-exceeded", "compile_units", "count"});

		auto value = std::make_shared<imported_project::implementation>();
		value->capture_bundle_digest = bundle.value_->digest;
		value->replay_plans.reserve(bundle.value_->projection.compile_units.size());
		for (std::size_t index{}; index < bundle.value_->projection.compile_units.size(); ++index)
		{
			auto plan_value = make_gcc_replay_plan(
				*bundle.value_, bundle.value_->projection.compile_units[index], index, limits);
			if (!plan_value)
				return unexpected(std::move(plan_value.error()));
			replay_plan plan{std::move(*plan_value)};
			value->unresolved.insert(
				value->unresolved.end(), plan.unresolved().begin(), plan.unresolved().end());
			value->replay_plans.push_back(std::move(plan));
		}
		canonicalize_gaps(value->unresolved);
		std::vector<canonical_value> fields;
		fields.reserve(value->replay_plans.size() + 1U);
		fields.push_back(canonical_value::from_string(value->capture_bundle_digest));
		for (const auto& plan : value->replay_plans)
			fields.push_back(canonical_value::from_string(std::string{plan.digest()}));
		auto identity = canonical_identity_digest("application-imported-project", fields);
		if (!identity)
			return unexpected(
				error{"application-analysis.capture-invalid", "imported_project", "identity"});
		value->id = "imported-project:" + *identity;
		return imported_project{std::move(value)};
	}

	materialization_request::materialization_request(std::shared_ptr<const implementation> value)
		: value_{std::move(value)}
	{
	}

	result<materialization_request>
	materialization_request::make(relation_engine engine,
								  snapshot_draft publication,
								  std::vector<std::string> relation_descriptor_ids,
								  std::string interpretation,
								  provider::provider_selection_request provider,
								  provider::execution_budget budget,
								  const std::stop_token& cancellation)
	{
		if (relation_descriptor_ids.empty())
			return unexpected(invalid("relation_descriptor_ids", "empty"));
		std::set<std::string, std::less<>> unique;
		for (const auto& descriptor_id : relation_descriptor_ids)
		{
			if (!unique.insert(descriptor_id).second)
				return unexpected(invalid("relation_descriptor_ids", "duplicate"));
			if (auto descriptor = engine.require_id(descriptor_id); !descriptor)
				return unexpected(invalid("relation_descriptor_ids", "unknown"));
		}
		if (interpretation.empty())
			return unexpected(invalid("interpretation", "empty"));
		if (auto valid = publication.series.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (publication.snapshot_semantics_version.major == 0U ||
			!digest_like(publication.catalog_semantic_digest) ||
			(publication.expected_parent_publication &&
			 !digest_like(*publication.expected_parent_publication)))
			return unexpected(invalid("publication", "authority"));
		if (provider.provider_id.empty() || provider.provider_version.major == 0U ||
			!digest_like(provider.provider_binary_digest) ||
			!digest_like(provider.provider_semantic_contract_digest))
			return unexpected(invalid("provider", "identity"));
		if (auto valid = provider.sandbox.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (provider.fallback_policy)
			if (auto valid = provider.fallback_policy->validate(provider.provider_version); !valid)
				return unexpected(std::move(valid.error()));
		if (auto valid = budget.validate(); !valid)
			return unexpected(std::move(valid.error()));

		auto value =
			std::make_shared<implementation>(implementation{std::move(engine),
															std::move(publication),
															std::move(relation_descriptor_ids),
															std::move(interpretation),
															std::move(provider),
															budget,
															cancellation});
		return materialization_request{std::move(value)};
	}

	std::span<const std::string> materialization_request::relation_descriptor_ids() const noexcept
	{
		return value_->relation_descriptor_ids;
	}
	std::string_view materialization_request::interpretation() const noexcept
	{
		return value_->interpretation;
	}
	const provider::execution_budget& materialization_request::budget() const noexcept
	{
		return value_->budget;
	}

	materialization_result::materialization_result(std::shared_ptr<const implementation> value)
		: value_{std::move(value)}
	{
	}

	materialization_terminal materialization_result::terminal() const noexcept
	{
		return value_->terminal;
	}
	const std::optional<snapshot_handle>&
	materialization_result::published_snapshot() const noexcept
	{
		return value_->published_snapshot;
	}
	std::span<const provider::coverage_unit> materialization_result::coverage() const noexcept
	{
		return value_->coverage;
	}
	std::span<const provider::unresolved_item> materialization_result::unresolved() const noexcept
	{
		return value_->unresolved;
	}
	std::span<const claim_conflict> materialization_result::conflicts() const noexcept
	{
		return value_->conflicts;
	}
	std::span<const differential_disagreement>
	materialization_result::differential_disagreements() const noexcept
	{
		return value_->differential_disagreements;
	}
	const std::optional<application_analysis_provenance>&
	materialization_result::provenance() const noexcept
	{
		return value_->provenance;
	}

	result<materialization_result>
	materialize(snapshot_store&, const imported_project&, const materialization_request& request)
	{
		if (request.value_->cancellation.stop_requested())
		{
			auto value = std::make_shared<materialization_result::implementation>();
			value->terminal = materialization_terminal::cancelled;
			return materialization_result{std::move(value)};
		}
		return unexpected(error{"application-analysis.target-unavailable",
								"materialization",
								"application analysis providers are not configured"});
	}
} // namespace cxxlens::sdk
