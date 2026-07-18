#include <algorithm>
#include <array>
#include <map>
#include <ranges>
#include <set>
#include <tuple>

#include <cxxlens/sdk/incremental.hpp>

namespace cxxlens::sdk::incremental
{
	namespace
	{
		[[nodiscard]] error make_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool digest_like(const std::string_view value)
		{
			std::size_t hex_offset{};
			if (value.starts_with("sha256:"))
				hex_offset = 7U;
			else
			{
				const auto marker = value.rfind(":sha256:");
				if (marker == std::string_view::npos || marker == 0U)
					return false;
				const auto prefix = value.substr(0U, marker);
				if (!std::ranges::all_of(prefix,
										 [](const char byte)
										 {
											 return (byte >= 'a' && byte <= 'z') ||
												 (byte >= '0' && byte <= '9') || byte == '.' ||
												 byte == '-' || byte == '_';
										 }))
					return false;
				hex_offset = marker + 8U;
			}
			const auto hex = value.substr(hex_offset);
			return hex.size() == 64U &&
				std::ranges::all_of(hex,
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		template <class T>
		void canonical_set(std::vector<T>& values)
		{
			std::ranges::sort(values);
			values.erase(std::ranges::unique(values).begin(), values.end());
		}

		[[nodiscard]] canonical_value strings(const std::span<const std::string> values)
		{
			std::vector<canonical_value> fields;
			fields.reserve(values.size());
			for (const auto& value : values)
				fields.push_back(canonical_value::from_string(value));
			return canonical_value::from_tuple(std::move(fields));
		}

		[[nodiscard]] result<std::string> digest_plan(const std::span<const plan_entry> entries,
													  const std::uint64_t executions,
													  const bool warm_zero)
		{
			std::vector<canonical_value> encoded;
			encoded.reserve(entries.size());
			for (const auto& entry : entries)
				encoded.push_back(canonical_value::from_tuple(
					{canonical_value::from_string(entry.partition_id),
					 canonical_value::from_string(entry.decision == action::reuse ? "reuse"
																				  : "recompute"),
					 canonical_value::from_string(entry.reason)}));
			const std::array fields{
				canonical_value::from_tuple(std::move(encoded)),
				canonical_value::from_integer(static_cast<std::int64_t>(executions)),
				canonical_value::from_boolean(warm_zero),
			};
			return canonical_identity_digest("materialization-plan", fields);
		}

		[[nodiscard]] std::string changed_reason(const input_fingerprint& prior,
												 const input_fingerprint& current)
		{
			const std::array comparisons{
				std::pair{&input_fingerprint::source_digest, "sdk.incremental-source-changed"},
				std::pair{&input_fingerprint::dependency_digest,
						  "sdk.incremental-dependency-changed"},
				std::pair{&input_fingerprint::invocation_digest,
						  "sdk.incremental-invocation-changed"},
				std::pair{&input_fingerprint::toolchain_digest,
						  "sdk.incremental-toolchain-changed"},
				std::pair{&input_fingerprint::condition_universe_digest,
						  "sdk.incremental-condition-universe-changed"},
				std::pair{&input_fingerprint::variant_digest, "sdk.incremental-variant-changed"},
				std::pair{&input_fingerprint::provider_set_digest,
						  "sdk.incremental-provider-set-changed"},
				std::pair{&input_fingerprint::registry_digest, "sdk.incremental-registry-changed"},
				std::pair{&input_fingerprint::interpretation_policy_digest,
						  "sdk.incremental-interpretation-policy-changed"},
				std::pair{&input_fingerprint::refresh_policy_digest,
						  "sdk.incremental-refresh-policy-changed"},
				std::pair{&input_fingerprint::environment_digest,
						  "sdk.incremental-environment-changed"},
				std::pair{&input_fingerprint::provider_binary_digest,
						  "sdk.incremental-provider-binary-changed"},
				std::pair{&input_fingerprint::provider_semantics_digest,
						  "sdk.incremental-provider-semantics-changed"},
				std::pair{&input_fingerprint::relation_descriptor_digest,
						  "sdk.incremental-relation-descriptor-changed"},
				std::pair{&input_fingerprint::normalizer_version,
						  "sdk.incremental-normalizer-changed"},
				std::pair{&input_fingerprint::model_digest, "sdk.incremental-model-changed"},
				std::pair{&input_fingerprint::assumption_digest,
						  "sdk.incremental-assumption-changed"},
				std::pair{&input_fingerprint::precision_profile,
						  "sdk.incremental-precision-changed"},
			};
			for (const auto& [member, reason] : comparisons)
				if (prior.*member != current.*member)
					return reason;
			return {};
		}

		[[nodiscard]] std::vector<std::string> intersection(std::vector<std::string> left,
															std::vector<std::string> right)
		{
			canonical_set(left);
			canonical_set(right);
			std::vector<std::string> output;
			std::ranges::set_intersection(left, right, std::back_inserter(output));
			return output;
		}

		[[nodiscard]] bool row_less(const closure_row& left, const closure_row& right)
		{
			return std::tie(left.root,
							left.target,
							left.interpretation,
							left.condition_fragments,
							left.evidence) < std::tie(right.root,
													  right.target,
													  right.interpretation,
													  right.condition_fragments,
													  right.evidence);
		}

		[[nodiscard]] result<std::string> digest_closure(const closure_result& value)
		{
			std::vector<canonical_value> rows;
			for (const auto& row : value.rows)
				rows.push_back(
					canonical_value::from_tuple({canonical_value::from_string(row.root),
												 canonical_value::from_string(row.target),
												 strings(row.condition_fragments),
												 canonical_value::from_string(row.interpretation),
												 strings(row.evidence)}));
			std::vector<canonical_value> unresolved;
			for (const auto& item : value.unresolved)
				unresolved.push_back(
					canonical_value::from_tuple({canonical_value::from_string(item.code),
												 canonical_value::from_string(item.field),
												 canonical_value::from_string(item.detail)}));
			const std::array fields{
				canonical_value::from_string(value.closure_kind),
				canonical_value::from_tuple(std::move(rows)),
				canonical_value::from_integer(static_cast<std::int64_t>(value.iterations)),
				canonical_value::from_boolean(value.closure_certified),
				canonical_value::from_tuple(std::move(unresolved)),
			};
			return canonical_identity_digest("bounded-closure-result", fields);
		}
	} // namespace

	result<void> input_fingerprint::validate() const
	{
		const std::array digest_fields{
			std::pair{std::string_view{source_digest}, "source_digest"},
			std::pair{std::string_view{dependency_digest}, "dependency_digest"},
			std::pair{std::string_view{invocation_digest}, "invocation_digest"},
			std::pair{std::string_view{toolchain_digest}, "toolchain_digest"},
			std::pair{std::string_view{condition_universe_digest}, "condition_universe_digest"},
			std::pair{std::string_view{variant_digest}, "variant_digest"},
			std::pair{std::string_view{provider_set_digest}, "provider_set_digest"},
			std::pair{std::string_view{registry_digest}, "registry_digest"},
			std::pair{std::string_view{interpretation_policy_digest},
					  "interpretation_policy_digest"},
			std::pair{std::string_view{refresh_policy_digest}, "refresh_policy_digest"},
			std::pair{std::string_view{environment_digest}, "environment_digest"},
			std::pair{std::string_view{provider_binary_digest}, "provider_binary_digest"},
			std::pair{std::string_view{provider_semantics_digest}, "provider_semantics_digest"},
			std::pair{std::string_view{relation_descriptor_digest}, "relation_descriptor_digest"},
			std::pair{std::string_view{model_digest}, "model_digest"},
			std::pair{std::string_view{assumption_digest}, "assumption_digest"},
		};
		for (const auto& [value, field] : digest_fields)
			if (!digest_like(value))
				return unexpected(make_error("sdk.incremental-digest-invalid", field));
		if (normalizer_version.empty() || precision_profile.empty())
			return unexpected(make_error("sdk.incremental-input-omitted",
										 normalizer_version.empty() ? "normalizer_version"
																	: "precision_profile"));
		return {};
	}

	result<std::string> input_fingerprint::digest() const
	{
		if (auto valid = validate(); !valid)
			return unexpected(std::move(valid.error()));
		const std::array fields{
			canonical_value::from_string(source_digest),
			canonical_value::from_string(dependency_digest),
			canonical_value::from_string(invocation_digest),
			canonical_value::from_string(toolchain_digest),
			canonical_value::from_string(condition_universe_digest),
			canonical_value::from_string(variant_digest),
			canonical_value::from_string(provider_set_digest),
			canonical_value::from_string(registry_digest),
			canonical_value::from_string(interpretation_policy_digest),
			canonical_value::from_string(refresh_policy_digest),
			canonical_value::from_string(environment_digest),
			canonical_value::from_string(provider_binary_digest),
			canonical_value::from_string(provider_semantics_digest),
			canonical_value::from_string(relation_descriptor_digest),
			canonical_value::from_string(normalizer_version),
			canonical_value::from_string(model_digest),
			canonical_value::from_string(assumption_digest),
			canonical_value::from_string(precision_profile),
		};
		return canonical_identity_digest("incremental-input", fields);
	}

	result<void> partition_state::validate() const
	{
		if (auto valid = validate_strong_id(partition_id); !valid)
			return unexpected(make_error("sdk.incremental-partition-invalid", "partition_id"));
		if (auto valid = input.validate(); !valid)
			return valid;
		if (!digest_like(coverage_digest) || !digest_like(closure_digest))
			return unexpected(
				make_error("sdk.incremental-digest-invalid",
						   !digest_like(coverage_digest) ? "coverage_digest" : "closure_digest"));
		return {};
	}

	result<void> materialization_plan::validate() const
	{
		if (entries.empty())
			return unexpected(make_error("sdk.incremental-plan-empty", "entries"));
		if (!std::ranges::is_sorted(entries, {}, &plan_entry::partition_id))
			return unexpected(make_error("sdk.incremental-plan-order", "entries"));
		std::set<std::string, std::less<>> ids;
		std::uint64_t recomputed{};
		static constexpr std::array<std::string_view, 22U> recompute_reasons{
			"sdk.incremental-no-prior",
			"sdk.incremental-corruption-detected",
			"sdk.incremental-source-changed",
			"sdk.incremental-dependency-changed",
			"sdk.incremental-invocation-changed",
			"sdk.incremental-toolchain-changed",
			"sdk.incremental-condition-universe-changed",
			"sdk.incremental-variant-changed",
			"sdk.incremental-provider-set-changed",
			"sdk.incremental-registry-changed",
			"sdk.incremental-interpretation-policy-changed",
			"sdk.incremental-refresh-policy-changed",
			"sdk.incremental-environment-changed",
			"sdk.incremental-provider-binary-changed",
			"sdk.incremental-provider-semantics-changed",
			"sdk.incremental-relation-descriptor-changed",
			"sdk.incremental-normalizer-changed",
			"sdk.incremental-model-changed",
			"sdk.incremental-assumption-changed",
			"sdk.incremental-precision-changed",
			"sdk.incremental-coverage-changed",
			"sdk.incremental-closure-changed",
		};
		for (const auto& entry : entries)
		{
			if ((entry.decision != action::reuse && entry.decision != action::recompute) ||
				!validate_strong_id(entry.partition_id) || !ids.insert(entry.partition_id).second ||
				(entry.decision == action::reuse &&
				 entry.reason != "sdk.incremental-exact-reuse") ||
				(entry.decision == action::recompute &&
				 !std::ranges::contains(recompute_reasons, entry.reason)))
				return unexpected(
					make_error("sdk.incremental-plan-entry-invalid", entry.partition_id));
			if (entry.decision == action::recompute)
				++recomputed;
		}
		if (frontend_provider_executions != recomputed || warm_zero != (recomputed == 0U))
			return unexpected(make_error("sdk.incremental-plan-accounting", "warm_zero"));
		auto expected = digest_plan(entries, frontend_provider_executions, warm_zero);
		if (!expected || *expected != plan_digest)
			return unexpected(make_error("sdk.incremental-plan-digest", "plan_digest"));
		return {};
	}

	result<materialization_plan>
	make_materialization_plan(const std::span<const partition_candidate> candidates)
	{
		if (candidates.empty())
			return unexpected(make_error("sdk.incremental-candidates-empty", "candidates"));
		materialization_plan output;
		for (const auto& candidate : candidates)
		{
			if (auto valid = candidate.current.validate(); !valid)
				return unexpected(std::move(valid.error()));
			plan_entry entry{candidate.current.partition_id, action::recompute, {}};
			if (!candidate.prior)
				entry.reason = "sdk.incremental-no-prior";
			else
			{
				if (auto valid = candidate.prior->validate(); !valid)
					return unexpected(std::move(valid.error()));
				if (candidate.prior->partition_id != candidate.current.partition_id)
					return unexpected(make_error("sdk.incremental-partition-mismatch",
												 candidate.current.partition_id));
				if (candidate.prior->corruption_detected || candidate.current.corruption_detected)
					entry.reason = "sdk.incremental-corruption-detected";
				else if (auto reason =
							 changed_reason(candidate.prior->input, candidate.current.input);
						 !reason.empty())
					entry.reason = std::move(reason);
				else if (candidate.prior->coverage_digest != candidate.current.coverage_digest)
					entry.reason = "sdk.incremental-coverage-changed";
				else if (candidate.prior->closure_digest != candidate.current.closure_digest)
					entry.reason = "sdk.incremental-closure-changed";
				else
				{
					entry.decision = action::reuse;
					entry.reason = "sdk.incremental-exact-reuse";
				}
			}
			output.entries.push_back(std::move(entry));
		}
		std::ranges::sort(output.entries, {}, &plan_entry::partition_id);
		if (std::ranges::adjacent_find(output.entries, {}, &plan_entry::partition_id) !=
			output.entries.end())
			return unexpected(make_error("sdk.incremental-duplicate-partition", "candidates"));
		output.frontend_provider_executions = static_cast<std::uint64_t>(
			std::ranges::count(output.entries, action::recompute, &plan_entry::decision));
		output.warm_zero = output.frontend_provider_executions == 0U;
		auto digest =
			digest_plan(output.entries, output.frontend_provider_executions, output.warm_zero);
		if (!digest)
			return unexpected(std::move(digest.error()));
		output.plan_digest = std::move(*digest);
		return output;
	}

	result<void> closure_edge::validate() const
	{
		if (!validate_strong_id(from))
			return unexpected(make_error("sdk.closure-edge-invalid", "from"));
		if (!validate_strong_id(to))
			return unexpected(make_error("sdk.closure-edge-invalid", "to"));
		if (condition_fragments.empty() || !validate_strong_id(interpretation) || evidence.empty())
			return unexpected(make_error("sdk.closure-edge-incomplete", from, to));
		for (const auto& fragment : condition_fragments)
			if (!validate_strong_id(fragment))
				return unexpected(make_error("sdk.closure-condition-invalid", from, fragment));
		for (const auto& item : evidence)
			if (!validate_strong_id(item))
				return unexpected(make_error("sdk.closure-evidence-invalid", from, item));
		return {};
	}

	result<void> closure_request::validate() const
	{
		if (roots.empty() || max_iterations == 0U || max_edges == 0U)
			return unexpected(make_error("sdk.closure-request-invalid", "budget"));
		static constexpr std::array<std::string_view, 3U> derived_kinds{
			"call-target-set",
			"inheritance-subtype-set",
			"include-provider-set",
		};
		if (!std::ranges::contains(derived_kinds, closure_kind))
			return unexpected(make_error("sdk.closure-kind-invalid", "closure_kind", closure_kind));
		for (const auto& root : roots)
			if (!validate_strong_id(root))
				return unexpected(make_error("sdk.closure-root-invalid", "roots", root));
		for (const auto& edge : edges)
			if (auto valid = edge.validate(); !valid)
				return valid;
		return {};
	}

	result<void> closure_result::validate() const
	{
		if (closure_kind != "call-target-set" && closure_kind != "inheritance-subtype-set" &&
			closure_kind != "include-provider-set")
			return unexpected(make_error("sdk.closure-kind-invalid", "closure_kind", closure_kind));
		if (!std::ranges::is_sorted(rows, row_less))
			return unexpected(make_error("sdk.closure-result-order", "rows"));
		if (closure_certified == !unresolved.empty())
			return unexpected(make_error("sdk.closure-certification-invalid", "unresolved"));
		for (const auto& row : rows)
			if (!validate_strong_id(row.root) || !validate_strong_id(row.target) ||
				row.condition_fragments.empty() || !validate_strong_id(row.interpretation) ||
				row.evidence.empty())
				return unexpected(make_error("sdk.closure-row-incomplete", row.root, row.target));
			else if (std::ranges::any_of(row.condition_fragments,
										 [](const std::string& value)
										 {
											 return !validate_strong_id(value);
										 }) ||
					 std::ranges::any_of(row.evidence,
										 [](const std::string& value)
										 {
											 return !validate_strong_id(value);
										 }))
				return unexpected(make_error("sdk.closure-row-incomplete", row.root, row.target));
		for (const auto& item : unresolved)
			if ((item.code != "sdk.closure-edge-budget" || item.field != "max_edges") &&
				(item.code != "sdk.closure-iteration-budget" || item.field != "max_iterations"))
				return unexpected(make_error("sdk.closure-certification-invalid", "unresolved"));
		if (std::ranges::adjacent_find(rows) != rows.end())
			return unexpected(make_error("sdk.closure-result-duplicate", "rows"));
		auto expected = digest_closure(*this);
		if (!expected || *expected != result_digest)
			return unexpected(make_error("sdk.closure-result-digest", "result_digest"));
		return {};
	}

	result<closure_result> bounded_transitive_closure(const closure_request& request)
	{
		if (auto valid = request.validate(); !valid)
			return unexpected(std::move(valid.error()));
		auto roots = request.roots;
		canonical_set(roots);
		auto edges = request.edges;
		for (auto& edge : edges)
		{
			canonical_set(edge.condition_fragments);
			canonical_set(edge.evidence);
		}
		std::ranges::sort(edges,
						  [](const closure_edge& left, const closure_edge& right)
						  {
							  return std::tie(left.from,
											  left.to,
											  left.interpretation,
											  left.condition_fragments,
											  left.evidence) < std::tie(right.from,
																		right.to,
																		right.interpretation,
																		right.condition_fragments,
																		right.evidence);
						  });
		closure_result output;
		output.closure_kind = request.closure_kind;
		if (edges.size() > request.max_edges)
		{
			edges.resize(static_cast<std::size_t>(request.max_edges));
			output.unresolved.push_back(
				{"sdk.closure-edge-budget", "max_edges", std::to_string(request.max_edges)});
		}
		std::vector<closure_row> frontier;
		for (const auto& root : roots)
			for (const auto& edge : edges)
				if (edge.from == root)
					frontier.push_back({root,
										edge.to,
										edge.condition_fragments,
										edge.interpretation,
										edge.evidence});
		std::map<std::tuple<std::string, std::string, std::string, std::vector<std::string>>,
				 closure_row>
			reached;
		bool converged = frontier.empty();
		while (!frontier.empty() && output.iterations < request.max_iterations)
		{
			++output.iterations;
			std::vector<closure_row> next;
			for (auto row : frontier)
			{
				canonical_set(row.condition_fragments);
				canonical_set(row.evidence);
				auto key =
					std::tuple{row.root, row.target, row.interpretation, row.condition_fragments};
				auto [position, inserted] = reached.emplace(key, row);
				if (!inserted)
				{
					const auto old_size = position->second.evidence.size();
					position->second.evidence.insert(
						position->second.evidence.end(), row.evidence.begin(), row.evidence.end());
					canonical_set(position->second.evidence);
					if (position->second.evidence.size() == old_size)
						continue;
				}
				for (const auto& edge : edges)
				{
					if (edge.from != row.target || edge.interpretation != row.interpretation)
						continue;
					auto condition =
						intersection(row.condition_fragments, edge.condition_fragments);
					if (condition.empty())
						continue;
					auto evidence = row.evidence;
					evidence.insert(evidence.end(), edge.evidence.begin(), edge.evidence.end());
					canonical_set(evidence);
					next.push_back({row.root,
									edge.to,
									std::move(condition),
									row.interpretation,
									std::move(evidence)});
				}
			}
			std::ranges::sort(next, row_less);
			frontier = std::move(next);
			converged = frontier.empty();
		}
		if (!converged)
			output.unresolved.push_back({"sdk.closure-iteration-budget",
										 "max_iterations",
										 std::to_string(request.max_iterations)});
		for (auto& [key, row] : reached)
		{
			(void)key;
			output.rows.push_back(std::move(row));
		}
		std::ranges::sort(output.rows, row_less);
		output.closure_certified = converged && output.unresolved.empty();
		auto digest = digest_closure(output);
		if (!digest)
			return unexpected(std::move(digest.error()));
		output.result_digest = std::move(*digest);
		return output;
	}
} // namespace cxxlens::sdk::incremental
