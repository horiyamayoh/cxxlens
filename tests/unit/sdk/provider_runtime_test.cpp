#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/sdk.hpp>

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;
	static_assert(!std::is_aggregate_v<provider_selection>);
	static_assert(std::is_same_v<decltype(std::declval<provider_selection&>().selected_candidate()),
								 const provider_candidate&>);

	constexpr std::string_view binary_digest =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	constexpr std::string_view semantic_contract_digest =
		"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	constexpr std::string_view policy_digest =
		"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] detached_row protocol_test_row()
	{
		using relation = cxxlens::company::relations::lock_acquire;
		relation::builder builder;
		require(builder
					.set<relation::acquire>(
						detached_cell::typed("company_lock_acquire_id", "lock-acquire:1"))
					.has_value(),
				"protocol row acquire setup failed");
		require(builder.set<relation::lock>(detached_cell::typed("company_lock_id", "lock:1"))
					.has_value(),
				"protocol row lock setup failed");
		require(builder.set<relation::source>(detached_cell::typed("source_span_id", "span:1"))
					.has_value(),
				"protocol row source setup failed");
		require(builder
					.set<relation::mode>(
						detached_cell{{scalar_kind::open_symbol, "company.lock-mode/1", false},
									  cell_state::present,
									  scalar_value{std::string{"exclusive"}},
									  std::nullopt})
					.has_value(),
				"protocol row mode setup failed");
		require(builder.set<relation::ordinal>(detached_cell::unsigned_integer(0U)).has_value(),
				"protocol row ordinal setup failed");
		auto row = std::move(builder).finish();
		require(row.has_value(), "protocol row validation failed");
		return std::move(*row);
	}

	class transcript_sink final : public frame_sink
	{
	  public:
		result<void> write(const std::span<const std::byte> bytes) override
		{
			transcript.insert(transcript.end(), bytes.begin(), bytes.end());
			return {};
		}

		std::vector<std::byte> transcript;
	};

	class parity_provider final : public portable_provider
	{
	  public:
		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "company.test.process-provider";
		}
		[[nodiscard]] semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		result<void> run(const cxxlens::sdk::provider::task& task_value,
						 cxxlens::sdk::provider::context& context) override
		{
			auto output = context.relation(cxxlens::company::relations::lock_acquire::descriptor());
			if (auto begun = output.begin("dependency-1", "atomic-1", "batch-1"); !begun)
				return begun;
			if (auto pushed = output.push(protocol_test_row()); !pushed)
				return pushed;
			if (auto pushed = output.push(protocol_test_row()); !pushed)
				return pushed;
			if (auto ended = output.end(); !ended)
				return ended;
			context.coverage().request("task", task_value.task_id);
			return context.coverage().classify({"task", task_value.task_id, "covered", {}});
		}
	};

	[[nodiscard]] std::string executable_digest(const std::string& executable)
	{
		std::ifstream input{executable, std::ios::binary};
		require(input.good(), "provider fixture could not be opened");
		const std::string bytes{std::istreambuf_iterator<char>{input},
								std::istreambuf_iterator<char>{}};
		require(!input.bad(), "provider fixture could not be read");
		return content_digest(std::as_bytes(std::span{bytes}));
	}

	[[nodiscard]] relation_descriptor snapshot_test_descriptor()
	{
		relation_descriptor value;
		value.id = "company.test.runtime_snapshot.v1";
		value.name = "company.test.runtime_snapshot";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.runtime_snapshot/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.runtime_snapshot.v1.key",
			 "key",
			 {scalar_kind::typed_id, "runtime_snapshot_id", false},
			 true,
			 column_role::claim_key},
		};
		value.key_columns = {"company.test.runtime_snapshot.v1.key"};
		value.merge = merge_mode::set;
		value.descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] detached_row snapshot_test_row()
	{
		auto descriptor = snapshot_test_descriptor();
		row_builder builder{descriptor};
		require(
			builder
				.set(
					{descriptor.id, descriptor.columns.front().id, descriptor.columns.front().type},
					detached_cell::typed("runtime_snapshot_id", "runtime:snapshot:1"))
				.has_value(),
			"snapshot test row setup failed");
		auto row = std::move(builder).finish();
		require(row.has_value(), "snapshot test row validation failed");
		return std::move(*row);
	}

	[[nodiscard]] partition_draft snapshot_test_partition(const relation_engine& engine)
	{
		observation observed{
			snapshot_test_row(),
			{"runtime-universe", {"all"}},
			"company.test.runtime-canonical-1",
			{"company.test.process-provider", std::string{binary_digest}},
			{"sha256:7777777777777777777777777777777777777777777777777777777777777777"},
			"evidence:runtime-snapshot",
			{"exact", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto assertion = make_assertion(engine, std::move(observed));
		require(assertion.has_value(), "snapshot test assertion failed");
		partition_draft draft;
		draft.relation_descriptor_id = snapshot_test_descriptor().id;
		draft.scope = "runtime-scope";
		draft.condition = {"runtime-universe", {"all"}};
		draft.interpretation = "company.test.runtime-canonical-1";
		draft.producer_semantics = binary_digest;
		draft.precision_profile = "exact";
		draft.assumption_set_id = "assumptions:none";
		draft.claims = {std::move(*assertion)};
		auto basis = claim_input_basis_digest(draft.claims.front().input_basis);
		require(basis.has_value(), "snapshot test input basis failed");
		draft.producer_input_basis_digest = std::move(*basis);
		draft.coverage = {{"runtime", "runtime-scope", "covered", ""}};
		return draft;
	}

	[[nodiscard]] manifest make_manifest(const semantic_version version = {1U, 0U, 0U},
										 std::string binary = std::string{binary_digest})
	{
		manifest value;
		value.provider_id = "company.test.process-provider";
		value.provider_version = version;
		value.package_identity = "company.test.process-provider.package";
		value.publisher = "company.test";
		value.license = "Apache-2.0";
		value.protocol = {1U, 0U, 0U, {"credit-backpressure"}, {}};
		value.platform_tuples = {"linux-glibc"};
		value.provider_binary_digest = std::move(binary);
		value.provider_semantic_contract_digest = semantic_contract_digest;
		value.offered_relations = {"company.lock.acquire@1"};
		value.interpretation_domains = {"provider.company.test.process-provider"};
		value.invalidation_contract =
			"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
		value.determinism_contract =
			"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
		value.resource_class = "provider.test";
		value.sandbox_minimum = "enforced";
		value.requested_qualifications = {"canonical-semantic-qualified"};
		return value;
	}

	[[nodiscard]] sandbox_report make_sandbox(const sandbox_assurance achieved)
	{
		return {"linux-glibc",
				{"no-shell-argv-exec"},
				achieved,
				std::string{policy_digest},
				"sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
	}

	[[nodiscard]] provider_candidate
	candidate(const std::string& executable,
			  const std::string& mode,
			  const discovery_source source = discovery_source::explicit_path,
			  const sandbox_assurance achieved = sandbox_assurance::enforced)
	{
		return {make_manifest({1U, 0U, 0U}, executable_digest(executable)),
				source,
				{executable, mode},
				true,
				true,
				true,
				{"canonical-semantic-qualified"},
				make_sandbox(achieved),
				{}};
	}

	[[nodiscard]] provider_selection_request selection_request(const std::string& executable)
	{
		return {"company.test.process-provider",
				{1U, 0U, 0U},
				executable_digest(executable),
				std::string{semantic_contract_digest},
				{sandbox_assurance::enforced, std::string{policy_digest}},
				true,
				std::nullopt};
	}

	[[nodiscard]] provider_fallback_tuple fallback_tuple(const provider_candidate& value,
														 const std::uint32_t priority,
														 const semantic_version requested = {
															 1U, 0U, 0U})
	{
		const auto version = value.description.provider_version;
		return {priority,
				value.description.provider_id,
				version,
				value.description.provider_binary_digest,
				value.description.provider_semantic_contract_digest,
				version > requested
					? fallback_direction::upgrade
					: (version < requested ? fallback_direction::downgrade
										   : fallback_direction::same_version_rebuild),
				true,
				{}};
	}

	[[nodiscard]] process_task_request task(provider_selection selection)
	{
		process_task_request request;
		request.selection = std::move(selection);
		request.output_descriptors = {cxxlens::company::relations::lock_acquire::descriptor()};
		request.task_id = "task-1";
		request.payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
		request.task_input_digest = content_digest(request.payload);
		request.normalized_invocation_digest =
			"sha256:1111111111111111111111111111111111111111111111111111111111111111";
		request.toolchain_digest =
			"sha256:2222222222222222222222222222222222222222222222222222222222222222";
		request.environment_digest =
			"sha256:3333333333333333333333333333333333333333333333333333333333333333";
		request.sandbox = {sandbox_assurance::enforced, std::string{policy_digest}};
		request.budget.wall_ms = 2000U;
		request.budget.cpu_ms = 2000U;
		request.budget.rss_bytes = 256U * 1024U * 1024U;
		request.budget.output_bytes = 4U * 1024U * 1024U;
		request.budget.open_files = 64U;
		request.budget.subprocesses = 1U;
		return request;
	}

	[[nodiscard]] provider_selection select(const std::string& executable, const std::string& mode)
	{
		const auto value = candidate(executable, mode);
		auto selected = select_provider(selection_request(executable), std::span{&value, 1U});
		require(selected.has_value(), "exact provider selection failed");
		return std::move(*selected);
	}

	void check_selection(const std::string& executable)
	{
		auto exact = candidate(executable, "success");
		auto selected = select_provider(selection_request(executable), std::span{&exact, 1U});
		require(selected && !selected->fallback_used() &&
					selected->decisions().front().reason == "selected-exact" &&
					selected->validate().has_value(),
				"exact selection was not explained");

		auto adjacent = exact;
		adjacent.description.provider_version = {1U, 1U, 0U};
		auto rejected = select_provider(selection_request(executable), std::span{&adjacent, 1U});
		require(!rejected && rejected.error().code == "provider.not-found",
				"adjacent provider version silently fell back");

		auto shadow = exact;
		shadow.description.package_identity = "company.test.shadow.package";
		shadow.description.provider_binary_digest =
			"sha256:9999999999999999999999999999999999999999999999999999999999999999";
		std::array shadowed{exact, shadow};
		auto ambiguous = select_provider(selection_request(executable), shadowed);
		require(!ambiguous && ambiguous.error().code == "security.provider-shadowing",
				"provider shadowing was first-wins selected");

		auto weak = candidate(
			executable, "success", discovery_source::explicit_path, sandbox_assurance::best_effort);
		auto unavailable = select_provider(selection_request(executable), std::span{&weak, 1U});
		require(!unavailable && unavailable.error().code == "security.downgrade-forbidden",
				"insufficient enforced sandbox was selected");

		auto path_only = exact;
		path_only.authoritative_path = false;
		auto path_rejected =
			select_provider(selection_request(executable), std::span{&path_only, 1U});
		require(!path_rejected && path_rejected.error().code == "security.downgrade-forbidden",
				"PATH-only provider discovery became authority");

		for (auto invalid :
			 {
				 [&]
				 {
					 auto value = exact;
					 value.trust_valid = false;
					 return value;
				 }(),
				 [&]
				 {
					 auto value = exact;
					 value.certification_valid = false;
					 return value;
				 }(),
				 [&]
				 {
					 auto value = exact;
					 value.validation_error = "security.signature-mismatch";
					 return value;
				 }(),
			 })
		{
			auto verdict_rejected =
				select_provider(selection_request(executable), std::span{&invalid, 1U});
			require(!verdict_rejected,
					"invalid trust/certification verdict produced a selection token");
		}

		auto fallback = exact;
		fallback.description.provider_version = {1U, 1U, 0U};
		auto fallback_request = selection_request(executable);
		fallback_request.fallback_policy = provider_fallback_policy{
			"company.test.provider-fallback-policy", {fallback_tuple(fallback, 1U)}};
		auto allowed = select_provider(fallback_request, std::span{&fallback, 1U});
		require(allowed && allowed->fallback_used() && allowed->fallback_policy_digest() &&
					allowed->selected_candidate().description.provider_version ==
						semantic_version{1U, 1U, 0U} &&
					allowed->canonical_form().contains(*allowed->fallback_policy_digest()),
				"exact fallback policy tuple was not selected or recorded");

		auto unrelated_major = fallback;
		unrelated_major.description.provider_version = {9U, 0U, 0U};
		auto major_rejected = select_provider(fallback_request, std::span{&unrelated_major, 1U});
		require(!major_rejected && major_rejected.error().code == "provider.not-found",
				"unlisted provider major was accepted by fallback policy");
		auto major_request = selection_request(executable);
		major_request.fallback_policy = provider_fallback_policy{
			"company.test.major-policy", {fallback_tuple(unrelated_major, 1U)}};
		auto major_allowed = select_provider(major_request, std::span{&unrelated_major, 1U});
		require(major_allowed && major_allowed->fallback_used(),
				"explicitly listed provider major fallback was rejected");

		auto rebuild = exact;
		rebuild.description.provider_binary_digest =
			"sha256:9999999999999999999999999999999999999999999999999999999999999999";
		auto rebuild_rejected = select_provider(fallback_request, std::span{&rebuild, 1U});
		require(!rebuild_rejected && rebuild_rejected.error().code == "provider.not-found",
				"unlisted same-version binary was accepted by fallback policy");
		auto rebuild_request = selection_request(executable);
		rebuild_request.fallback_policy =
			provider_fallback_policy{"company.test.rebuild-policy", {fallback_tuple(rebuild, 1U)}};
		auto rebuild_allowed = select_provider(rebuild_request, std::span{&rebuild, 1U});
		require(rebuild_allowed && rebuild_allowed->fallback_used(),
				"listed same-version rebuild tuple was rejected");

		auto semantic_change = fallback;
		semantic_change.description.provider_semantic_contract_digest =
			"sha256:8888888888888888888888888888888888888888888888888888888888888888";
		auto semantic_rejected = select_provider(fallback_request, std::span{&semantic_change, 1U});
		require(!semantic_rejected && semantic_rejected.error().code == "provider.not-found",
				"unlisted semantic contract was accepted by fallback policy");
		semantic_change.certified_qualifications.push_back("cross-version-qualified");
		auto semantic_entry = fallback_tuple(semantic_change, 1U);
		semantic_entry.required_qualifications = {"cross-version-qualified"};
		auto semantic_request = selection_request(executable);
		semantic_request.fallback_policy =
			provider_fallback_policy{"company.test.semantic-policy", {std::move(semantic_entry)}};
		auto semantic_allowed = select_provider(semantic_request, std::span{&semantic_change, 1U});
		require(semantic_allowed && semantic_allowed->fallback_used(),
				"qualified listed semantic contract fallback was rejected");
		auto self_claimed = semantic_change;
		self_claimed.certified_qualifications = {"canonical-semantic-qualified"};
		self_claimed.description.requested_qualifications.push_back("cross-version-qualified");
		auto self_claim_rejected = select_provider(semantic_request, std::span{&self_claimed, 1U});
		require(!self_claim_rejected && self_claim_rejected.error().code == "provider.not-found",
				"manifest self-claim substituted for certified fallback qualification");

		auto preferred = exact;
		preferred.description.provider_version = {1U, 2U, 0U};
		auto secondary = fallback;
		std::array fallback_candidates{secondary, preferred};
		auto precedence_request = selection_request(executable);
		precedence_request.fallback_policy = provider_fallback_policy{
			"company.test.precedence-policy",
			{fallback_tuple(secondary, 2U), fallback_tuple(preferred, 1U)}};
		auto precedence = select_provider(precedence_request, fallback_candidates);
		require(precedence &&
					precedence->selected_candidate().description.provider_version ==
						semantic_version{1U, 2U, 0U},
				"fallback policy priority did not define canonical selection");
		auto reversed_policy = *precedence_request.fallback_policy;
		std::ranges::reverse(reversed_policy.allowed);
		require(reversed_policy.canonical_form() ==
						precedence_request.fallback_policy->canonical_form() &&
					reversed_policy.semantic_digest() ==
						precedence_request.fallback_policy->semantic_digest(),
				"fallback policy identity depended on tuple input order");
	}

	void check_process_faults(const std::string& executable)
	{
		auto processes = make_system_provider_process_port();
		require(processes != nullptr, "system provider process port unavailable");
		process_provider_runtime runtime{*processes};
		auto forged = runtime.execute(task(provider_selection{}));
		require(!forged && forged.error().code == "provider.selection-invalid",
				"default/forged selection token reached process launch");

		for (const auto mode : {"success", "network-check"})
		{
			auto request = task(select(executable, mode));
			auto report = runtime.execute(request);
			require(report && report->succeeded() &&
						report->frames.front().type == message_type::hello &&
						report->frames.size() == 15U &&
						report->frames.at(1U).type == message_type::schema_negotiate &&
						report->frames.at(2U).type == message_type::task_accepted &&
						report->frames.at(3U).type == message_type::batch_begin &&
						report->frames.at(10U).type == message_type::batch_end &&
						report->frames.at(11U).type == message_type::coverage_chunk &&
						report->frames.back().type == message_type::task_complete &&
						report->sandbox.achieved == sandbox_assurance::enforced &&
						report->canonical_form().contains("cxxlens.provider-execution-report.v1"),
					std::string{"successful process provider failed: "} + mode +
						" terminal=" + (report ? report->terminal : report.error().code));
		}

		auto manifest_minimum_candidate = candidate(executable, "success");
		auto weaker_authority = selection_request(executable);
		weaker_authority.sandbox.minimum = sandbox_assurance::best_effort;
		auto manifest_minimum_selection =
			select_provider(weaker_authority, std::span{&manifest_minimum_candidate, 1U});
		require(manifest_minimum_selection.has_value(),
				"manifest-minimum provider selection failed");
		auto weakened_request = task(std::move(*manifest_minimum_selection));
		weakened_request.sandbox.minimum = sandbox_assurance::none;
		auto enforced = runtime.execute(weakened_request);
		require(enforced && enforced->succeeded() &&
					enforced->sandbox.achieved == sandbox_assurance::enforced,
				"runtime did not enforce max(selection, request, manifest) sandbox minimum");
		auto mismatched_policy = task(select(executable, "success"));
		mismatched_policy.sandbox.policy_digest =
			"sha256:9999999999999999999999999999999999999999999999999999999999999999";
		auto policy_rejected = runtime.execute(mismatched_policy);
		require(!policy_rejected &&
					policy_rejected.error().code == "security.sandbox-policy-mismatch",
				"runtime accepted a sandbox policy not bound by selection authority");

		auto optional = runtime.execute(task(select(executable, "optional-extension")));
		require(optional && optional->succeeded() && optional->frames.size() == 16U &&
					optional->frames.at(3U).flags ==
						static_cast<std::uint16_t>(frame_flag::optional_extension) &&
					static_cast<std::uint16_t>(optional->frames.at(3U).type) == 65000U,
				"unknown optional extension was not skipped with accounting evidence");
		auto optional_credit_request = task(select(executable, "optional-extension"));
		optional_credit_request.output_credit.frames = 15U;
		auto optional_credit = runtime.execute(optional_credit_request);
		require(optional_credit && optional_credit->terminal == "provider.credit-exceeded",
				"skipped optional extension was omitted from frame credit accounting");

		auto minor_candidate = candidate(executable, "success");
		minor_candidate.description.protocol.maximum_minor = 1U;
		auto minor_selection =
			select_provider(selection_request(executable), std::span{&minor_candidate, 1U});
		require(minor_selection.has_value(), "minor-capable provider selection failed");
		auto minor_request = task(std::move(*minor_selection));
		minor_request.limits.maximum_minor = 1U;
		auto negotiated_minor = runtime.execute(minor_request);
		require(
			negotiated_minor && negotiated_minor->succeeded() &&
				std::ranges::all_of(negotiated_minor->frames,
									[](const frame& value)
									{
										return value.protocol_minor == 1U;
									}),
			std::string{"session did not bind frames to the negotiated protocol minor: "} +
				(negotiated_minor ? negotiated_minor->terminal : negotiated_minor.error().code) +
				(negotiated_minor && !negotiated_minor->diagnostics.empty()
					 ? ":" + negotiated_minor->diagnostics.back().detail
					 : std::string{}));

		auto plain_transcript = runtime.execute(task(select(executable, "success")));
		auto eos_transcript = runtime.execute(task(select(executable, "success-eos")));
		require(plain_transcript && eos_transcript && plain_transcript->succeeded() &&
					eos_transcript->succeeded() &&
					eos_transcript->frames.back().flags ==
						static_cast<std::uint16_t>(frame_flag::end_of_stream) &&
					plain_transcript->semantic_digest() != eos_transcript->semantic_digest(),
				"frame flags were omitted from semantic transcript identity");

		auto failed = runtime.execute(task(select(executable, "failed")));
		require(failed && failed->terminal == "provider.schema-invalid",
				"provider task failure lost its structured terminal");

		auto crash = runtime.execute(task(select(executable, "crash")));
		require(crash && crash->terminal == "provider.crash" &&
					crash->termination_signal == SIGSEGV,
				"worker crash was not distinguished");

		auto timeout_request = task(select(executable, "timeout"));
		timeout_request.budget.wall_ms = 25U;
		auto timeout = runtime.execute(timeout_request);
		require(timeout && timeout->terminal == "provider.timeout",
				"worker timeout was not distinguished");

		auto cancelled_request = task(select(executable, "timeout"));
		std::stop_source cancelled;
		cancelled.request_stop();
		cancelled_request.cancellation = cancelled.get_token();
		auto cancellation = runtime.execute(cancelled_request);
		require(cancellation && cancellation->terminal == "provider.cancelled",
				"worker cancellation was not distinguished");

		auto malformed = runtime.execute(task(select(executable, "malformed")));
		require(malformed && malformed->terminal == "provider.truncated-stream",
				"malformed worker output was not distinguished");

		auto limited_request = task(select(executable, "output-limit"));
		limited_request.budget.output_bytes = 4096U;
		auto limited = runtime.execute(limited_request);
		require(limited && limited->terminal == "provider.output-limit",
				"worker output limit was not distinguished");

		auto identity = runtime.execute(task(select(executable, "wrong-identity")));
		require(identity && identity->terminal == "provider.binary-identity-mismatch",
				"worker identity mismatch was accepted");

		for (const auto& [mode, terminal] : std::array{
				 std::pair{"minimal", "provider.protocol-state-invalid"},
				 std::pair{"provider-credit", "provider.protocol-state-invalid"},
				 std::pair{"provider-open-task", "provider.protocol-state-invalid"},
				 std::pair{"provider-batch-ack", "provider.protocol-state-invalid"},
				 std::pair{"missing-accepted", "provider.protocol-state-invalid"},
				 std::pair{"wrong-task", "provider.task-binding-mismatch"},
				 std::pair{"unsealed-batch", "provider.protocol-state-invalid"},
				 std::pair{"inconsistent-batch", "provider.batch-invalid"},
				 std::pair{"bad-column", "provider.batch-invalid"},
				 std::pair{"column-length-mismatch", "provider.batch-invalid"},
				 std::pair{"reordered-column", "provider.batch-invalid"},
				 std::pair{"unknown-descriptor", "provider.relation-incompatible"},
				 std::pair{"incomplete-coverage", "provider.coverage-incomplete"},
				 std::pair{"bad-eos", "provider.protocol-state-invalid"},
				 std::pair{"invalid-utf8", "provider.protocol-state-invalid"},
				 std::pair{"nul-control", "provider.protocol-state-invalid"},
			 })
		{
			auto rejected = runtime.execute(task(select(executable, mode)));
			require(rejected && rejected->terminal == terminal && !rejected->succeeded(),
					std::string{"invalid provider transcript was accepted: "} + mode);
		}

		auto credit_request = task(select(executable, "success"));
		credit_request.output_credit.frames = 1U;
		auto credit = runtime.execute(credit_request);
		require(credit && credit->terminal == "provider.credit-exceeded" && !credit->succeeded(),
				"provider output exceeding granted frame credit was accepted");

		auto feature_candidate = candidate(executable, "success");
		feature_candidate.description.protocol.required_features = {"company.unsupported-feature"};
		auto feature_selection =
			select_provider(selection_request(executable), std::span{&feature_candidate, 1U});
		require(feature_selection.has_value(), "feature provider selection failed");
		auto feature_request = task(std::move(*feature_selection));
		auto feature = runtime.execute(feature_request);
		require(!feature && feature.error().code == "provider.required-feature-missing",
				"unsupported required provider feature was negotiated");

		auto process = runtime.execute(task(select(executable, "success")));
		require(process && process->succeeded(), "process parity transcript failed");
		transcript_sink sink;
		protocol_writer writer{sink};
		writer.grant_credit({64U * 1024U * 1024U, 65536U});
		parity_provider provider;
		const auto& descriptor = cxxlens::company::relations::lock_acquire::descriptor();
		const cxxlens::sdk::provider::task logical_task{
			"task-1",
			{"catalog", std::string{binary_digest}, ".", {"unit.cpp"}},
			{descriptor},
			"all",
			"company.test.canonical-1",
		};
		auto logical = run_worker(provider, logical_task, writer);
		require(logical.has_value(), "in-process logical provider stream failed");
		auto logical_frames = decode_frame_stream(sink.transcript);
		require(logical_frames && logical_frames->size() + 2U == process->frames.size(),
				"logical/wire provider transcript size diverged");
		auto aggregated = decode_column_chunk(
			logical_frames->at(2U).control, logical_frames->at(2U).payload, descriptor);
		require(aggregated && aggregated->row_count == 2U,
				"relation_sink did not aggregate bounded rows into a column chunk");
		for (std::size_t index = 0U; index < logical_frames->size(); ++index)
			require(logical_frames->at(index).type == process->frames.at(index + 2U).type,
					"logical/wire provider state transition diverged");
	}

	void check_prior_snapshot_preserved(const std::string& executable)
	{
		relation_registry registry;
		require(registry.add(snapshot_test_descriptor()).has_value(),
				"snapshot registry setup failed");
		auto engine = registry.build("provider-runtime-snapshot");
		require(engine.has_value(), "snapshot relation engine failed");
		auto store = make_in_memory_snapshot_store(*engine);
		require(store.has_value(), "snapshot store failed");
		snapshot_series_selector selector{
			"catalog",
			"scope",
			std::string{engine->generation()},
			"runtime-universe",
			std::string{engine->registry_digest()},
			"sha256:4444444444444444444444444444444444444444444444444444444444444444",
			"sha256:5555555555555555555555555555555555555555555555555555555555555555"};
		auto writer = store->begin(snapshot_draft{
			selector,
			{1U, 0U, 0U},
			"sha256:6666666666666666666666666666666666666666666666666666666666666666",
			std::nullopt});
		require(writer.has_value(), "snapshot writer failed");
		auto staged = writer->stage(snapshot_test_partition(*engine));
		require(staged.has_value(),
				"baseline snapshot staging failed: " +
					(staged ? std::string{} : staged.error().code + " " + staged.error().detail));
		require(writer->validate().has_value(), "baseline snapshot validation failed");
		auto published = writer->publish();
		require(published.has_value(), "baseline snapshot failed");
		const auto prior = published->manifest().id;

		auto processes = make_system_provider_process_port();
		process_provider_runtime runtime{*processes};
		auto failed = runtime.execute(task(select(executable, "crash")));
		require(failed && !failed->succeeded(), "crashing provider unexpectedly succeeded");
		auto current = store->current(selector);
		require(current && current->manifest().id == prior,
				"failed worker destroyed or replaced the prior snapshot");
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	require(argument_count == 2, "provider process fixture path missing");
	const std::string executable{arguments[1]};
	check_selection(executable);
	check_process_faults(executable);
	check_prior_snapshot_preserved(executable);
}
