#pragma once

/** @file provider.hpp @brief Portable provider authoring without manual wire framing. */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/relation.hpp>

namespace cxxlens::sdk::provider
{
	/** @brief Exact protocol v1 message types. */
	// The uint16 wire domain must retain unknown optional message IDs for accounting.
	enum class message_type : std::uint16_t // NOLINT(performance-enum-size)
	{
		hello = 1,
		hello_ack = 2,
		schema_negotiate = 3,
		open_task = 4,
		task_accepted = 5,
		input_descriptor = 6,
		input_chunk = 7,
		credit = 8,
		batch_begin = 9,
		column_chunk = 10,
		batch_end = 11,
		batch_ack = 12,
		batch_reject = 13,
		coverage_chunk = 14,
		unresolved_chunk = 15,
		closure_candidate = 16,
		progress = 17,
		cancel = 18,
		resume = 19,
		task_complete = 20,
		task_failed = 21,
		close = 22,
	};

	/** @brief Closed provider frame flag bits from the protocol v1 header. */
	enum class frame_flag : std::uint8_t
	{
		required_extension = 1U,
		optional_extension = 2U,
		compressed_payload = 4U,
		end_of_stream = 8U,
	};

	/** @brief Exact negotiated protocol version, supported flags, and frame limits. */
	struct protocol_limits
	{
		std::uint32_t max_control_bytes{65536U};
		std::uint64_t max_payload_bytes{16777216U};
		std::uint16_t protocol_major{1U};
		std::uint16_t minimum_minor{};
		std::uint16_t maximum_minor{};
		std::uint16_t supported_flags{static_cast<std::uint16_t>(frame_flag::end_of_stream)};
	};

	/** @brief Decoded frame retaining all semantic header fields after validation. */
	struct frame
	{
		message_type type{message_type::hello};
		std::uint64_t stream_id{};
		std::uint64_t sequence{};
		std::vector<std::byte> control;
		std::vector<std::byte> payload;
		std::uint16_t protocol_major{1U};
		std::uint16_t protocol_minor{};
		std::uint16_t flags{};
	};

	/** @brief Encode one 104-byte-header protocol frame with independent SHA-256 checksums. */
	[[nodiscard]] result<std::vector<std::byte>> encode_frame(const frame& value,
															  protocol_limits limits = {});
	/**
	 * @brief Decode, bound-check, and verify exactly one protocol frame.
	 * @details The negotiated minor range and flags fail closed. Unknown optional message types are
	 * retained for byte/frame accounting and must be skipped by the session state validator.
	 */
	[[nodiscard]] result<frame> decode_frame(std::span<const std::byte> input,
											 protocol_limits limits = {});
	/** @brief Decode a bounded concatenated transcript without accepting trailing fragments. */
	[[nodiscard]] result<std::vector<frame>>
	decode_frame_stream(std::span<const std::byte> input,
						protocol_limits limits = {},
						std::uint64_t maximum_frames = 65536U);
	/** @brief Decode one deterministic-CBOR text control value. */
	[[nodiscard]] result<std::string> decode_control_text(std::span<const std::byte> control);

	/** @brief Streaming byte destination implemented by process or in-memory transports. */
	class frame_sink
	{
	  public:
		virtual ~frame_sink() = default;
		[[nodiscard]] virtual result<void> write(std::span<const std::byte> frame_bytes) = 0;
	};

	/** @brief Strongly grouped byte/frame credit grant. */
	struct protocol_credit
	{
		std::uint64_t bytes{};
		std::uint64_t frames{};
	};

	/** @brief Credit-aware frame writer that owns sequence and checksum bookkeeping. */
	class protocol_writer
	{
	  public:
		explicit protocol_writer(frame_sink& sink, protocol_limits limits = {});
		void grant_credit(protocol_credit amount) noexcept;
		[[nodiscard]] result<void> send(message_type type,
										std::span<const std::byte> control = {},
										std::span<const std::byte> payload = {},
										std::uint16_t flags = 0U);
		[[nodiscard]] std::uint64_t remaining_bytes() const noexcept;
		[[nodiscard]] std::uint64_t remaining_frames() const noexcept;

	  private:
		frame_sink* sink_{};
		protocol_limits limits_;
		std::uint64_t stream_id_{1U};
		std::uint64_t sequence_{};
		std::uint64_t credit_bytes_{};
		std::uint64_t credit_frames_{};
	};

	/** @brief One requested coverage unit. */
	struct coverage_unit
	{
		std::string kind;
		std::string id;
		std::string state;
		std::string reason;
	};

	/** @brief Balanced execution coverage builder. */
	class coverage_builder
	{
	  public:
		coverage_builder& request(std::string kind, std::string id);
		[[nodiscard]] result<void> classify(coverage_unit unit);
		[[nodiscard]] result<std::vector<coverage_unit>> finish() &&;

	  private:
		std::vector<std::pair<std::string, std::string>> requested_;
		std::vector<coverage_unit> units_;
	};

	/** @brief Structured unresolved item emitted alongside partial results. */
	struct unresolved_item
	{
		std::string code;
		std::string subject;
		std::string detail;

		[[nodiscard]] bool operator==(const unresolved_item&) const = default;
	};

	/** @brief Canonical unresolved builder. */
	class unresolved_builder
	{
	  public:
		unresolved_builder& add(unresolved_item item);
		[[nodiscard]] result<std::vector<unresolved_item>> finish() &&;

	  private:
		std::vector<unresolved_item> items_;
	};

	/** @brief Structured evidence item; prose is not an identity input. */
	struct evidence_item
	{
		std::string kind;
		std::string subject;
		std::string producer;
		std::string summary;

		[[nodiscard]] bool operator==(const evidence_item&) const = default;
	};

	/** @brief Canonical evidence builder. */
	class evidence_builder
	{
	  public:
		evidence_builder& add(evidence_item item);
		[[nodiscard]] result<std::vector<evidence_item>> finish() &&;

	  private:
		std::vector<evidence_item> items_;
	};

	/** @brief Exact portable provider task; all values are detached. */
	struct task
	{
		std::string task_id;
		project_catalog project;
		std::vector<relation_descriptor> outputs;
		std::string condition;
		std::string interpretation;
	};

	/** @brief Host-enforced provider budget; zero is never an implicit unlimited value. */
	struct execution_budget
	{
		std::uint64_t wall_ms{60000U};
		std::uint64_t cpu_ms{60000U};
		std::uint64_t rss_bytes{536870912U};
		std::uint64_t output_bytes{16777216U};
		std::uint64_t rows{100000U};
		std::uint64_t diagnostics{10000U};
		std::uint64_t open_files{1024U};
		std::uint64_t created_files{1024U};
		std::uint64_t subprocesses{1U};
	};

	/** @brief Task execution controls owned by the host. */
	struct execution_context
	{
		std::stop_token cancellation;
		execution_budget budget;
	};

	class context;

	/** @brief High-level relation stream; providers never construct protocol frames. */
	class relation_sink
	{
	  public:
		[[nodiscard]] result<void>
		begin(std::string dependency_group, std::string atomic_output_group, std::string batch_id);
		[[nodiscard]] result<void> push(const detached_row& row);
		[[nodiscard]] result<void> end();
		[[nodiscard]] std::uint64_t row_count() const noexcept;

	  private:
		relation_sink(protocol_writer& writer,
					  relation_descriptor descriptor,
					  std::uint64_t& total_rows,
					  std::uint64_t maximum_rows);
		protocol_writer* writer_{};
		relation_descriptor descriptor_;
		std::string dependency_group_;
		std::string atomic_output_group_;
		std::string batch_id_;
		std::string rolling_digest_;
		std::uint64_t row_count_{};
		std::uint64_t* total_rows_{};
		std::uint64_t maximum_rows_{};
		bool open_{};
		friend class context;
	};

	/** @brief Provider callback context with streaming and partiality helpers. */
	class context
	{
	  public:
		context(protocol_writer& writer, execution_context execution);
		[[nodiscard]] relation_sink relation(relation_descriptor descriptor);
		[[nodiscard]] coverage_builder& coverage() noexcept;
		[[nodiscard]] unresolved_builder& unresolved() noexcept;
		[[nodiscard]] evidence_builder& evidence() noexcept;
		[[nodiscard]] bool stop_requested() const noexcept;

	  private:
		protocol_writer* writer_{};
		execution_context execution_;
		coverage_builder coverage_;
		unresolved_builder unresolved_;
		evidence_builder evidence_;
		std::uint64_t output_rows_{};
	};

	/** @brief Portable provider interface using detached values only. */
	class portable_provider
	{
	  public:
		virtual ~portable_provider() = default;
		[[nodiscard]] virtual std::string_view id() const noexcept = 0;
		[[nodiscard]] virtual semantic_version version() const noexcept = 0;
		[[nodiscard]] virtual result<void> run(const task& task, context& context) = 0;
	};

	/** @brief Provider manifest request; trust and certification remain external authority. */
	struct protocol_range
	{
		std::uint32_t major{1U};
		std::uint32_t minimum_minor{};
		std::uint32_t maximum_minor{};
		std::vector<std::string> required_features;
		std::vector<std::string> optional_features;
	};

	/** @brief Exact provider-manifest v1 value with canonical set ordering. */
	struct manifest
	{
		std::string provider_id;
		semantic_version provider_version;
		std::string package_identity;
		std::string publisher;
		std::string license;
		std::optional<std::string> signature;
		protocol_range protocol;
		std::vector<std::string> platform_tuples;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		std::vector<std::string> offered_relations;
		std::vector<std::string> required_relations;
		std::vector<std::string> interpretation_domains;
		std::string invalidation_contract;
		std::string determinism_contract;
		std::string resource_class;
		std::string sandbox_minimum{"enforced"};
		std::vector<std::string> requested_qualifications;
		std::vector<std::string> trust_flags;
		std::string task_input_stage{"observation"};
		std::string task_output_stage{"observation"};
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string canonical_json() const;
	};

	/** @brief Ordered sandbox assurance achieved by a process transport. */
	enum class sandbox_assurance : std::uint8_t
	{
		none,
		best_effort,
		enforced,
		certified,
	};

	/** @brief Effective sandbox minimum and its versioned policy identity. */
	struct sandbox_requirement
	{
		sandbox_assurance minimum{sandbox_assurance::enforced};
		std::string policy_digest;
		[[nodiscard]] result<void> validate() const;
	};

	/** @brief Runtime sandbox evidence; manifest self-claims are never copied here. */
	struct sandbox_report
	{
		std::string platform;
		std::vector<std::string> mechanisms;
		sandbox_assurance achieved{sandbox_assurance::none};
		std::string policy_digest;
		std::string evidence_digest;
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string canonical_form() const;
	};

	/** @brief Deterministic provider discovery precedence. */
	enum class discovery_source : std::uint8_t
	{
		explicit_path,
		installation_manifest,
		project_config,
		system_registry,
	};

	/** @brief One discovered candidate before trust, compatibility, and sandbox selection. */
	struct provider_candidate
	{
		manifest description;
		discovery_source source{discovery_source::system_registry};
		std::vector<std::string> executable_argv;
		bool authoritative_path{};
		bool trust_valid{};
		bool certification_valid{};
		std::vector<std::string> certified_qualifications;
		sandbox_report sandbox;
		std::string validation_error;
	};

	/** @brief Direction of one exact, explicitly authorized fallback identity. */
	enum class fallback_direction : std::uint8_t
	{
		upgrade,
		downgrade,
		same_version_rebuild,
	};

	/** @brief One exact fallback tuple with explicit deterministic policy priority. */
	struct provider_fallback_tuple
	{
		std::uint32_t priority{};
		std::string provider_id;
		semantic_version provider_version;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		fallback_direction direction{fallback_direction::upgrade};
		bool require_certification{true};
		std::vector<std::string> required_qualifications;
		[[nodiscard]] result<void> validate(const semantic_version& requested_version) const;
		[[nodiscard]] std::string canonical_form() const;
	};

	/** @brief Named exact-tuple fallback policy; lower unique priority wins canonically. */
	struct provider_fallback_policy
	{
		std::string policy_id;
		std::vector<provider_fallback_tuple> allowed;
		[[nodiscard]] result<void> validate(const semantic_version& requested_version) const;
		[[nodiscard]] std::string canonical_form() const;
		[[nodiscard]] std::string semantic_digest() const;
	};

	/** @brief Exact requested provider identity with optional exact-tuple fallback policy. */
	struct provider_selection_request
	{
		std::string provider_id;
		semantic_version provider_version;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		sandbox_requirement sandbox;
		bool require_certification{true};
		std::optional<provider_fallback_policy> fallback_policy;
	};

	/** @brief Explainable decision for every discovered candidate. */
	struct provider_candidate_decision
	{
		discovery_source source{discovery_source::system_registry};
		std::string provider_id;
		semantic_version provider_version;
		std::string binary_digest;
		bool selected{};
		std::string reason;
		[[nodiscard]] bool operator==(const provider_candidate_decision&) const = default;
	};

	/** @brief Exact selected candidate plus complete rejection/fallback evidence. */
	struct provider_selection
	{
		provider_candidate candidate;
		std::vector<provider_candidate_decision> decisions;
		bool fallback_used{};
		std::optional<std::string> fallback_policy_digest;
		[[nodiscard]] std::string canonical_form() const;
	};

	/** @brief Select one exact provider without PATH authority or silent downgrade. */
	[[nodiscard]] result<provider_selection>
	select_provider(const provider_selection_request& request,
					std::span<const provider_candidate> candidates);

	/** @brief Process transport outcome before provider protocol validation. */
	enum class process_status : std::uint8_t
	{
		exited,
		timed_out,
		cancelled,
		output_limit,
		crashed,
		unavailable,
		launch_failed,
	};

	/** @brief Shell-free bounded process invocation owned by the provider runtime port. */
	struct process_invocation
	{
		std::vector<std::string> argv;
		std::vector<std::byte> standard_input;
		std::string working_directory;
		std::vector<std::pair<std::string, std::string>> environment;
		execution_budget budget;
		sandbox_requirement sandbox;
		std::string expected_binary_digest;
	};

	/** @brief Detached process result with achieved sandbox evidence. */
	struct process_output
	{
		process_status status{process_status::launch_failed};
		int exit_code{};
		int termination_signal{};
		std::vector<std::byte> standard_output;
		std::string standard_error;
		sandbox_report sandbox;
		std::string failure_code;
	};

	/** @brief Port isolating filesystem/process/platform effects from provider semantics. */
	class provider_process_port
	{
	  public:
		virtual ~provider_process_port() = default;
		[[nodiscard]] virtual result<process_output> run(const process_invocation& invocation,
														 std::stop_token cancellation) const = 0;
	};

	/** @brief Create the Linux production argv/process-group/sandbox transport. */
	[[nodiscard]] std::unique_ptr<provider_process_port> make_system_provider_process_port();

	/** @brief Opaque provider task payload plus exact semantic/cache identity binding. */
	struct process_task_request
	{
		provider_selection selection;
		/** Exact relation schemas authorized for provider output and batch validation. */
		std::vector<relation_descriptor> output_descriptors;
		std::string task_id;
		std::vector<std::byte> payload;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		sandbox_requirement sandbox;
		execution_budget budget;
		protocol_limits limits;
		protocol_credit output_credit{67108864U, 65536U};
		std::stop_token cancellation;
	};

	/** @brief Structured process-provider execution report with validated transcript. */
	struct process_execution_report
	{
		std::string terminal;
		manifest provider;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		sandbox_report sandbox;
		std::vector<frame> frames;
		std::vector<unresolved_item> diagnostics;
		int exit_code{};
		int termination_signal{};
		[[nodiscard]] bool succeeded() const noexcept;
		[[nodiscard]] std::string canonical_form() const;
		[[nodiscard]] std::string semantic_digest() const;
	};

	/**
	 * @brief Backend-independent typed protocol validator for one process-provider task.
	 * @details Success requires negotiated identity/schema, exact task binding, granted credit,
	 * authorized descriptor and row shape, sealed batch digests, complete side channels, and a
	 * task-bound terminal. Invalid direction or state transitions fail closed.
	 */
	class process_provider_runtime
	{
	  public:
		explicit process_provider_runtime(const provider_process_port& processes);
		[[nodiscard]] result<process_execution_report>
		execute(const process_task_request& request) const;

	  private:
		const provider_process_port* processes_{};
	};

	/** @brief Rendered provider project file without direct filesystem effects. */
	struct scaffold_file
	{
		std::string relative_path;
		std::string content;
	};

	/** @brief Provider scaffold request. */
	struct scaffold_options
	{
		std::string provider_id;
		std::string provider_class{"portable"};
		std::string relation_name;
	};

	/** @brief Generate CMake, manifest, source, test, and README files in memory. */
	[[nodiscard]] result<std::vector<scaffold_file>> make_scaffold(const scaffold_options& options);

	/** @brief Run one task through the protocol server; framing and terminal messages are owned. */
	[[nodiscard]] result<void> run_worker(portable_provider& provider,
										  const task& task,
										  protocol_writer& writer,
										  execution_context execution = {});
} // namespace cxxlens::sdk::provider
