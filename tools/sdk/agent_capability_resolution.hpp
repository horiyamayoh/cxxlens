#pragma once

#include <array>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk::doctor_detail
{
	inline constexpr std::string_view relation_path_json =
		R"json([{"disposition":"implemented","evidence":["source-closure-capture"],"id":"input.source-snapshot.v1","kind":"input","owner_issue":"#261","requires":[],"state":"proved"},{"disposition":"implemented","evidence":["relation-registry","sdk-query"],"id":"relation.cc-call-site.v1","kind":"relation","owner_issue":"#66","requires":["input.source-snapshot.v1"],"state":"proved"},{"disposition":"implemented","evidence":["coverage","provenance"],"id":"evidence.relation-provenance.v1","kind":"evidence","owner_issue":"#66","requires":["relation.cc-call-site.v1"],"state":"proved"}])json";
	inline constexpr std::string_view recipe_path_json =
		R"json([{"disposition":"partial","evidence":["compile-command-binding"],"id":"input.effective-invocation.v1","kind":"input","owner_issue":"#261","requires":[],"state":"partial"},{"disposition":"partial","evidence":["provider-task","materialization-report"],"id":"provider.clang22-materialization.v2_1","kind":"provider","owner_issue":"#261","requires":["input.effective-invocation.v1"],"state":"partial"},{"disposition":"implemented","evidence":["recipe-plan","logical-explain"],"id":"recipe.explain-translation-unit.v1","kind":"recipe","owner_issue":"#261","requires":["provider.clang22-materialization.v2_1"],"state":"disproved"}])json";
	inline constexpr std::string_view analysis_path_json =
		R"json([{"disposition":"implemented","evidence":["snapshot-identity"],"id":"artifact.semantic-snapshot.v1","kind":"input","owner_issue":"#181","requires":[],"state":"proved"},{"disposition":"implementation-tracked","evidence":["coverage","unresolved"],"id":"analysis.coverage-aware.v1","kind":"analysis","owner_issue":"#277","requires":["artifact.semantic-snapshot.v1"],"state":"conflicting"}])json";
	inline constexpr std::string_view model_path_json =
		R"json([{"disposition":"implemented","evidence":["toolchain-context"],"id":"input.toolchain-context.v1","kind":"input","owner_issue":"#66","requires":[],"state":"proved"},{"disposition":"missing","evidence":[],"id":"model.assumption-pack.v1","kind":"model","owner_issue":"#257","requires":["input.toolchain-context.v1"],"state":"unknown"}])json";
	inline constexpr std::string_view portable_provider_path_json =
		R"json([{"disposition":"implemented","evidence":["portable-provider-task"],"id":"input.portable-task.v1","kind":"input","owner_issue":"#66","requires":[],"state":"proved"},{"disposition":"implemented","evidence":["provider-protocol","sdk-contract"],"id":"provider.portable-sdk.v1","kind":"provider","owner_issue":"#66","requires":["input.portable-task.v1"],"state":"proved"},{"disposition":"implemented","evidence":["typed-transcript","deterministic-output"],"id":"evidence.provider-transcript.v1","kind":"evidence","owner_issue":"#66","requires":["provider.portable-sdk.v1"],"state":"proved"}])json";
	inline constexpr std::string_view native_provider_path_json =
		R"json([{"disposition":"partial","evidence":["toolchain-provenance"],"id":"input.native-toolchain.v1","kind":"input","owner_issue":"#261","requires":[],"state":"partial"},{"disposition":"partial","evidence":["native-provider","source-closure-vfs"],"id":"provider.clang22-native.v1","kind":"provider","owner_issue":"#261","requires":["input.native-toolchain.v1"],"state":"partial"},{"disposition":"tracked-gap","evidence":[],"id":"evidence.native-install-qualified.v1","kind":"evidence","owner_issue":"#173","requires":["provider.clang22-native.v1"],"state":"unknown"}])json";
	inline constexpr std::string_view query_operator_path_json =
		R"json([{"disposition":"implemented","evidence":["relation-registry"],"id":"relation.queryable.v1","kind":"relation","owner_issue":"#66","requires":[],"state":"proved"},{"disposition":"implemented","evidence":["logical-query-ir","static-dynamic-parity"],"id":"query.operator.v1","kind":"query","owner_issue":"#66","requires":["relation.queryable.v1"],"state":"proved"}])json";
	inline constexpr std::string_view support_tuple_path_json =
		R"json([{"disposition":"conformance-only","evidence":["support-matrix"],"id":"support.provider-tuple.v1","kind":"support","owner_issue":"#173","requires":[],"state":"unknown"}])json";
	inline constexpr std::string_view actionable_unknown_path_json =
		R"json([{"disposition":"implemented","evidence":["use-case-catalog"],"id":"input.admitted-use-case.v1","kind":"input","owner_issue":"#277","requires":[],"state":"proved"},{"disposition":"blocked","evidence":["work-unit-registry"],"id":"capability.dependency-graph.v1","kind":"evidence","owner_issue":"#173","requires":["input.admitted-use-case.v1"],"state":"unknown"}])json";

	inline constexpr std::string_view recipe_missing_json =
		R"json([{"capability_id":"input.effective-invocation.v1","completion_plan":["completion.authority-evidence.v1"],"explanation":"input.effective-invocation.v1 is partial in the exact capability authority.","owner_issue":"#261","reason_code":"missing-input"},{"capability_id":"provider.clang22-materialization.v2_1","completion_plan":["completion.authority-evidence.v1"],"explanation":"provider.clang22-materialization.v2_1 is partial in the exact capability authority.","owner_issue":"#261","reason_code":"blocked-dependency"}])json";
	inline constexpr std::string_view analysis_missing_json =
		R"json([{"capability_id":"analysis.coverage-aware.v1","completion_plan":["completion.analysis-corpus.v1"],"explanation":"analysis.coverage-aware.v1 is conflicting in the exact capability authority.","owner_issue":"#277","reason_code":"conflicting-evidence"}])json";
	inline constexpr std::string_view model_missing_json =
		R"json([{"capability_id":"model.assumption-pack.v1","completion_plan":["completion.model-pack.v1"],"explanation":"model.assumption-pack.v1 is unknown in the exact capability authority.","owner_issue":"#257","reason_code":"missing-model"}])json";
	inline constexpr std::string_view native_provider_missing_json =
		R"json([{"capability_id":"input.native-toolchain.v1","completion_plan":["completion.native-installed-e2e.v1"],"explanation":"input.native-toolchain.v1 is partial in the exact capability authority.","owner_issue":"#261","reason_code":"missing-input"},{"capability_id":"provider.clang22-native.v1","completion_plan":["completion.native-installed-e2e.v1"],"explanation":"provider.clang22-native.v1 is partial in the exact capability authority.","owner_issue":"#261","reason_code":"blocked-dependency"},{"capability_id":"evidence.native-install-qualified.v1","completion_plan":["completion.native-installed-e2e.v1"],"explanation":"evidence.native-install-qualified.v1 is unknown in the exact capability authority.","owner_issue":"#173","reason_code":"blocked-dependency"}])json";
	inline constexpr std::string_view support_tuple_missing_json =
		R"json([{"capability_id":"support.provider-tuple.v1","completion_plan":["completion.support-gr-tuple.v1"],"explanation":"support.provider-tuple.v1 is unknown in the exact capability authority.","owner_issue":"#173","reason_code":"missing-evidence"}])json";
	inline constexpr std::string_view actionable_unknown_missing_json =
		R"json([{"capability_id":"capability.dependency-graph.v1","completion_plan":["completion.governance-receipt.v1"],"explanation":"capability.dependency-graph.v1 is unknown in the exact capability authority.","owner_issue":"#173","reason_code":"blocked-dependency"}])json";

	inline constexpr std::string_view analysis_completion_plan_json =
		R"json([{"action":"Add an accepted analysis implementation and negative corpus for every partial branch.","depends_on":[],"id":"completion.analysis-corpus.v1","owner_issue":"#277","unlocks":["analysis.coverage-aware.v1"]}])json";
	inline constexpr std::string_view model_completion_plan_json =
		R"json([{"action":"Register the exact model pack, assumptions, version, and qualification evidence.","depends_on":[],"id":"completion.model-pack.v1","owner_issue":"#257","unlocks":["model.assumption-pack.v1"]}])json";
	inline constexpr std::string_view native_provider_completion_plan_json =
		R"json([{"action":"Produce exact installed static/shared real-project evidence for the selected tuple.","depends_on":[],"id":"completion.native-installed-e2e.v1","owner_issue":"#173","unlocks":["evidence.native-install-qualified.v1"]}])json";
	inline constexpr std::string_view support_tuple_completion_plan_json =
		R"json([{"action":"Bind the exact provider, relation, toolchain, platform, binary, and evidence digests to a GR report.","depends_on":[],"id":"completion.support-gr-tuple.v1","owner_issue":"#173","unlocks":["support.provider-tuple.v1"]}])json";
	inline constexpr std::string_view actionable_unknown_completion_plan_json =
		R"json([{"action":"Accept the exact governance receipt and complete predecessor work units before selecting execution.","depends_on":[],"id":"completion.governance-receipt.v1","owner_issue":"#173","unlocks":["capability.dependency-graph.v1"]}])json";

	struct golden_capability
	{
		std::string_view id;
		std::string_view consumer;
		std::string_view question;
		std::string_view kind;
		std::string_view state;
		std::string_view reason;
		std::string_view disposition;
		std::string_view owner_issue;
		std::string_view evidence;
		std::string_view path_json;
		std::string_view missing_json;
		std::string_view completion_plan_json;
	};

	inline constexpr std::array golden_capabilities{
		golden_capability{
			"agent.golden-relation.v1",
			"relation-inspector",
			"Can the admitted relation be queried with its identity and provenance intact?",
			"relation",
			"proved",
			"none",
			"implemented",
			"#66",
			"relation-registry",
			relation_path_json,
			"[]",
			"[]"},
		golden_capability{
			"agent.golden-recipe.v1",
			"recipe-author",
			"Can a versioned recipe produce a deterministic, evidence-backed explanation?",
			"recipe",
			"disproved",
			"conflicting-evidence",
			"disproved",
			"#261",
			"recipe-plan",
			recipe_path_json,
			recipe_missing_json,
			"[]"},
		golden_capability{"agent.golden-analysis.v1",
						  "analysis-author",
						  "Can an analysis retain partiality and unresolved findings instead of "
						  "collapsing them to success?",
						  "analysis",
						  "conflicting",
						  "conflicting-evidence",
						  "implementation-tracked",
						  "#277",
						  "coverage",
						  analysis_path_json,
						  analysis_missing_json,
						  analysis_completion_plan_json},
		golden_capability{
			"agent.golden-model.v1",
			"model-pack-author",
			"Is the requested model pack available for this analysis and toolchain context?",
			"model",
			"unknown",
			"missing-model",
			"missing",
			"#257",
			"",
			model_path_json,
			model_missing_json,
			model_completion_plan_json},
		golden_capability{
			"agent.golden-portable-provider.v1",
			"portable-provider-author",
			"Can a portable provider publish typed observations through the versioned protocol?",
			"provider",
			"proved",
			"none",
			"implemented",
			"#66",
			"provider-protocol",
			portable_provider_path_json,
			"[]",
			"[]"},
		golden_capability{
			"agent.golden-native-provider.v1",
			"native-provider-author",
			"Is the native provider path qualified for the requested toolchain and platform tuple?",
			"provider",
			"partial",
			"missing-input",
			"partial",
			"#261",
			"toolchain-provenance",
			native_provider_path_json,
			native_provider_missing_json,
			native_provider_completion_plan_json},
		golden_capability{
			"agent.golden-query-operator.v1",
			"query-author",
			"Can the query operator preserve typed identity, order, and static/dynamic parity?",
			"query",
			"proved",
			"none",
			"implemented",
			"#66",
			"logical-query-ir",
			query_operator_path_json,
			"[]",
			"[]"},
		golden_capability{"agent.golden-support-tuple.v1",
						  "package-selector",
						  "Is this provider/relation/toolchain/platform tuple explicitly supported "
						  "and qualified?",
						  "support",
						  "unknown",
						  "missing-evidence",
						  "conformance-only",
						  "#173",
						  "support-matrix",
						  support_tuple_path_json,
						  support_tuple_missing_json,
						  support_tuple_completion_plan_json},
		golden_capability{"agent.golden-actionable-unknown.v1",
						  "autonomous-coding-agent",
						  "If completion is unavailable, can the agent receive an actionable typed "
						  "reason and dependency-ordered upgrade plan?",
						  "evidence",
						  "unknown",
						  "blocked-dependency",
						  "blocked",
						  "#173",
						  "work-unit-registry",
						  actionable_unknown_path_json,
						  actionable_unknown_missing_json,
						  actionable_unknown_completion_plan_json},
	};

	[[nodiscard]] inline std::string json_escape(const std::string_view value)
	{
		std::string output;
		output.reserve(value.size() + 2U);
		for (const char byte : value)
		{
			switch (byte)
			{
				case '\\':
					output += "\\\\";
					break;
				case '"':
					output += "\\\"";
					break;
				case '\n':
					output += "\\n";
					break;
				case '\r':
					output += "\\r";
					break;
				case '\t':
					output += "\\t";
					break;
				default:
					output += byte;
					break;
			}
		}
		return output;
	}

	[[nodiscard]] inline const golden_capability* find_golden(const std::string_view id) noexcept
	{
		for (const auto& capability : golden_capabilities)
			if (capability.id == id)
				return &capability;
		return nullptr;
	}

	[[nodiscard]] inline std::string revision()
	{
#ifdef CXXLENS_SOURCE_REVISION
		return CXXLENS_SOURCE_REVISION;
#else
		return std::string(40U, '0');
#endif
	}

	[[nodiscard]] inline std::string tree()
	{
#ifdef CXXLENS_SOURCE_TREE
		return CXXLENS_SOURCE_TREE;
#else
		return std::string(40U, '0');
#endif
	}

	[[nodiscard]] inline std::string static_authority_digest(const std::string_view name)
	{
		std::vector<std::byte> bytes;
		bytes.reserve(name.size());
		for (const auto byte : name)
			bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return content_digest(bytes);
	}

	[[nodiscard]] inline bool valid_identifier(const std::string_view value) noexcept
	{
		if (value.empty() || value.front() < 'a' || value.front() > 'z')
			return false;
		bool separator_seen = false;
		bool segment_has_character = false;
		for (const char byte : value)
		{
			if (byte == '.' || byte == '-')
			{
				if (!segment_has_character)
					return false;
				separator_seen = true;
				segment_has_character = false;
				continue;
			}
			if ((byte < 'a' || byte > 'z') && (byte < '0' || byte > '9') && byte != '_')
				return false;
			segment_has_character = true;
		}
		return separator_seen && segment_has_character;
	}

	[[nodiscard]] inline std::string result_exit_state(const std::string_view state) noexcept
	{
		return state == "proved" || state == "disproved" ? "accepted" : "safe-stop";
	}

	[[nodiscard]] inline std::string resolution_json(const std::string_view requested_id)
	{
		const auto* selected = find_golden(requested_id);
		const bool known = selected != nullptr;
		const auto unknown_id = std::string_view{"capability.unknown-use-case.v1"};
		const auto id = known				 ? selected->id
			: valid_identifier(requested_id) ? requested_id
											 : "unknown.use-case.v1";
		const auto consumer = known ? selected->consumer : "unknown-consumer";
		const auto question = known
			? selected->question
			: "The requested use case is not admitted by the exact capability authority.";
		const auto kind = known ? selected->kind : "evidence";
		const auto state = known ? selected->state : "unknown";
		const auto reason = known ? selected->reason : "unknown-use-case";
		const auto disposition = known ? selected->disposition : "unknown-use-case";
		const auto owner = known ? selected->owner_issue : "#277";
		const auto capability_id = known ? selected->id : unknown_id;
		const auto plan_id =
			known ? "completion.authority-evidence.v1" : "completion.admit-use-case.v1";
		const auto explanation = state == "proved"
			? "Every capability in the selected path has an accepted positive result."
			: state == "disproved"
			? "The selected path has a typed negative result; no success is inferred."
			: state == "partial"
			? "The selected path is usable only for the explicitly proven subset."
			: state == "conflicting"
			? "Independent evidence disagrees; the result remains conflicting."
			: (known ? "The selected path cannot currently prove the requested question."
					 : "No admitted use-case entry exists; no capability is inferred from surface "
					   "presence.");
		const auto guarantee = state == "proved" ? "path-complete-for-declared-inputs-and-evidence"
			: state == "disproved"	 ? "negative-result-is-explicit-and-preserves-evidence"
			: state == "partial"	 ? "partial-result-retains-coverage-and-unresolved-fields"
			: state == "conflicting" ? "conflict-is-not-reduced-to-success-or-empty"
									 : "unknown-result-has-actionable-missing-reason-and-plan";
		const bool authority_evidence_available = state == "proved" || state == "disproved";

		std::ostringstream body;
		body << "{\"authority\":{\"authority_digest\":\""
			 << static_authority_digest("agent-capability-resolution") << "\",\"revision\":\""
			 << revision()
			 << "\",\"source\":\"schemas/cxxlens_ng_agent_capability_resolution.yaml\","
			 << "\"source_digest\":\""
			 << static_authority_digest("schemas/cxxlens_ng_agent_capability_resolution.yaml")
			 << "\",\"stale_policy\":\"reject\",\"tree\":\"" << tree() << "\"},"
			 << "\"capability_path\":";
		if (known)
			body << selected->path_json;
		else
			body << "[{\"disposition\":\"" << disposition << "\",\"evidence\":[],\"id\":\""
				 << json_escape(capability_id) << "\",\"kind\":\"" << kind
				 << "\",\"owner_issue\":\"" << owner << "\",\"requires\":[],\"state\":\"" << state
				 << "\"}]";
		body << ",\"completion_plan\":";
		if (known)
			body << selected->completion_plan_json;
		else
			body << "[{\"action\":\"Resolve the exact capability authority blocker.\","
				 << "\"depends_on\":[],\"id\":\"" << plan_id << "\",\"owner_issue\":\"" << owner
				 << "\",\"unlocks\":[\"" << json_escape(capability_id) << "\"]}]";
		body << ","
			 << "\"consumer\":\"" << json_escape(consumer)
			 << "\",\"document_version\":\"1.0.0\",\"evidence\":{\"available\":[";
		if (authority_evidence_available)
			body << "\"authority-digest\"";
		body << "],\"missing\":[";
		if (!authority_evidence_available)
			body << "\"authority-digest\"";
		body << "],\"required\":[\"authority-digest\"]},"
			 << "\"expected_result_states\":[\"proved\",\"disproved\",\"unknown\",\"partial\","
				"\"conflicting\"],";
		if (known)
			body << "\"missing\":" << selected->missing_json;
		else
		{
			body << "\"missing\":[{\"capability_id\":\"" << json_escape(capability_id)
				 << "\",\"completion_plan\":[\"" << plan_id << "\"],\"explanation\":\""
				 << explanation << "\",\"owner_issue\":\"" << owner << "\",\"reason_code\":\""
				 << reason << "\"}]";
		}
		body << ",\"preserved_semantics\":{\"closure\":[\"preserved:closure\"],"
			 << "\"conflict\":[\"preserved:conflict\"],\"coverage\":[\"preserved:coverage\"],"
			 << "\"differential_disagreement\":[\"preserved:differential-disagreement\"],"
			 << "\"guarantee\":[\"preserved:guarantee\"],\"logical_explain\":["
				"\"preserved:logical-explain\"],\"physical_explain\":[\"preserved:physical-"
				"explain\"],"
			 << "\"provenance\":[\"preserved:provenance\"],\"unresolved\":[\"preserved:"
				"unresolved\"]},"
			 << "\"provenance\":{\"generated_at_revision\":\"" << revision()
			 << "\",\"generated_at_tree\":\"" << tree()
			 << "\",\"generator\":\"tools/sdk/sdk_doctor_main.cpp\","
			 << "\"input_contract\":\"cxxlens.agent-capability-resolution.v1\"},"
			 << "\"question\":\"" << json_escape(question) << "\",\"result\":{\"explanation\":\""
			 << explanation << "\",\"guarantee\":\"" << guarantee << "\",\"reason_code\":\""
			 << reason << "\",\"state\":\"" << state
			 << "\"},\"role\":\"canonical-capability-resolution\","
			 << "\"schema\":\"cxxlens.agent-capability-resolution.v1\",\"use_case_id\":\""
			 << json_escape(id) << "\"";
		const auto without_digest = body.str() + "}";
		std::vector<std::byte> bytes;
		bytes.reserve(without_digest.size());
		for (const auto byte : without_digest)
			bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		body << ",\"canonical_digest\":\"" << content_digest(bytes) << "\"}";
		return body.str();
	}

	[[nodiscard]] inline bool read_text_file(const std::string_view path, std::string& output)
	{
		std::ifstream input{std::string{path}, std::ios::binary};
		if (!input)
			return false;
		std::ostringstream stream;
		stream << input.rdbuf();
		output = stream.str();
		return true;
	}

	[[nodiscard]] inline std::string markdown_projection(const std::string_view json)
	{
		return std::string{"# cxxlens capability resolution\n\n"} +
			"The following JSON is the canonical resolution projection.\n\n```json\n" +
			std::string{json} + "\n```\n";
	}
} // namespace cxxlens::sdk::doctor_detail
