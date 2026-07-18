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
	namespace detail
	{
		struct relation_sink_registry;
	} // namespace detail

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
	/** @brief Test exact membership in the closed individual frame flag enum. */
	[[nodiscard]] constexpr bool is_valid(const frame_flag value) noexcept
	{
		return value == frame_flag::required_extension || value == frame_flag::optional_extension ||
			value == frame_flag::compressed_payload || value == frame_flag::end_of_stream;
	}

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
	/**
	 * @brief Encode one strictly valid UTF-8 string as deterministic CBOR text.
	 * @details Valid Unicode scalar bytes, including NUL, are preserved without normalization.
	 */
	[[nodiscard]] result<std::vector<std::byte>> encode_control_text(std::string_view text);
	/**
	 * @brief Decode one deterministic-CBOR text control value after strict UTF-8 validation.
	 * @details Typed control schemas decide whether valid code points such as NUL are permitted.
	 */
	[[nodiscard]] result<std::string> decode_control_text(std::span<const std::byte> control);

	/** @brief One typed column chunk before or after portable wire encoding. */
	struct column_chunk_record
	{
		std::string task_id;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::string descriptor_id;
		std::string descriptor_digest;
		std::string column_id;
		std::uint64_t row_offset{};
		std::uint32_t row_count{};
		std::uint64_t chunk_index{};
		std::string encoding;
		std::vector<detached_cell> cells;
		std::string chunk_digest;
	};

	/** @brief Deterministic CBOR control and portable binary payload for one column chunk. */
	struct encoded_column_chunk
	{
		std::vector<std::byte> control;
		std::vector<std::byte> payload;
		std::string chunk_digest;
	};

	/** @brief Encode one descriptor-bound column chunk with validity and unknown bitsets. */
	[[nodiscard]] result<encoded_column_chunk> encode_column_chunk(const column_chunk_record& value,
																   const column_descriptor& column);
	/** @brief Decode and independently validate one descriptor-bound column chunk. */
	[[nodiscard]] result<column_chunk_record>
	decode_column_chunk(std::span<const std::byte> control,
						std::span<const std::byte> payload,
						const column_descriptor& column);
	/** @brief Resolve the encoded column id against a relation descriptor and decode it. */
	[[nodiscard]] result<column_chunk_record>
	decode_column_chunk(std::span<const std::byte> control,
						std::span<const std::byte> payload,
						const relation_descriptor& descriptor);

	/** @brief Descriptor-order cumulative accounting for one completed batch column. */
	struct batch_column_summary
	{
		std::string column_id;
		std::uint64_t payload_bytes{};
		std::uint64_t chunk_count{};
		[[nodiscard]] bool operator==(const batch_column_summary&) const = default;
	};

	/** @brief Exact terminal binding for a columnar relation batch. */
	struct columnar_batch_end
	{
		std::string task_id;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::string descriptor_id;
		std::string descriptor_digest;
		std::uint64_t row_count{};
		std::vector<batch_column_summary> columns;
		std::vector<std::string> ordered_chunk_digests;
		std::string batch_digest;
	};

	/** @brief Deterministic CBOR control and binary ordered summary for batch end. */
	struct encoded_columnar_batch_end
	{
		std::vector<std::byte> control;
		std::vector<std::byte> payload;
	};

	/** @brief Digest exact named fields and ordered collections as a typed canonical tuple. */
	[[nodiscard]] std::string columnar_batch_digest(const columnar_batch_end& value);
	/** @brief Encode exact column lengths, order, chunk digests, and typed batch digest. */
	[[nodiscard]] result<encoded_columnar_batch_end>
	encode_columnar_batch_end(const columnar_batch_end& value);
	/** @brief Decode exact column lengths, order, chunk digests, and batch digest. */
	[[nodiscard]] result<columnar_batch_end>
	decode_columnar_batch_end(std::span<const std::byte> control,
							  std::span<const std::byte> payload);

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

	/** @brief Structured evidence item whose summary is retained in transcript identity. */
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

	/** @brief Exact provider identity and task binding carried by task_accepted. */
	struct task_accepted_metadata
	{
		std::string provider_id;
		std::string provider_version;
		std::string task_id;
		[[nodiscard]] bool operator==(const task_accepted_metadata&) const = default;
	};

	/** @brief Exact relation and group binding carried by batch_begin. */
	struct batch_begin_metadata
	{
		std::string task_id;
		std::string descriptor_id;
		std::string descriptor_digest;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		[[nodiscard]] bool operator==(const batch_begin_metadata&) const = default;
	};

	/** @brief Exact successful terminal binding carried by task_complete. */
	struct task_complete_metadata
	{
		std::string task_id;
		[[nodiscard]] bool operator==(const task_complete_metadata&) const = default;
	};

	/** @brief Exact failed terminal binding carried by task_failed. */
	struct task_failed_metadata
	{
		std::string error_code;
		std::string task_id;
		std::string error_field;
		[[nodiscard]] bool operator==(const task_failed_metadata&) const = default;
	};

	/** @brief Exact protocol schema and minor carried by schema_negotiate. */
	struct schema_negotiate_metadata
	{
		std::string protocol_schema;
		std::uint64_t protocol_minor{};
		[[nodiscard]] bool operator==(const schema_negotiate_metadata&) const = default;
	};

	/** @brief Exact task input and environment binding carried by open_task. */
	struct open_task_metadata
	{
		std::string task_id;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		[[nodiscard]] bool operator==(const open_task_metadata&) const = default;
	};

	/** @brief Exact host-granted byte and frame credit. */
	struct credit_metadata
	{
		std::uint64_t bytes{};
		std::uint64_t frames{};
		[[nodiscard]] bool operator==(const credit_metadata&) const = default;
	};

	/** @brief Exact task binding carried by close. */
	struct close_metadata
	{
		std::string task_id;
		[[nodiscard]] bool operator==(const close_metadata&) const = default;
	};

	/** @brief Independent authority expected in a host-to-provider task transcript. */
	struct host_transcript_expectation
	{
		std::string provider_manifest;
		open_task_metadata task;
		protocol_limits limits;
	};

	/** @brief Complete host-to-provider transcript input owned by the encoder. */
	struct host_transcript_request
	{
		host_transcript_expectation expectation;
		protocol_credit credit;
		std::vector<std::byte> payload;
	};

	/** @brief Validated task, credit, and payload returned to a provider worker. */
	struct validated_host_transcript
	{
		open_task_metadata task;
		protocol_credit credit;
		std::vector<std::byte> payload;
	};

	/** @brief Encode/decode exact deterministic CBOR metadata for task_accepted. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_task_accepted_metadata(const task_accepted_metadata& value);
	/** @brief Decode and validate exact task_accepted CBOR metadata. */
	[[nodiscard]] result<task_accepted_metadata>
	decode_task_accepted_metadata(std::span<const std::byte> control);

	/** @brief Encode/decode exact deterministic CBOR metadata for batch_begin. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_batch_begin_metadata(const batch_begin_metadata& value);
	/** @brief Decode and validate exact batch_begin CBOR metadata. */
	[[nodiscard]] result<batch_begin_metadata>
	decode_batch_begin_metadata(std::span<const std::byte> control);

	/** @brief Encode/decode a canonical coverage record set with explicit record count. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_coverage_metadata(std::span<const coverage_unit> values);
	/** @brief Decode canonical coverage records without losing text bytes. */
	[[nodiscard]] result<std::vector<coverage_unit>>
	decode_coverage_metadata(std::span<const std::byte> control);

	/** @brief Encode/decode a canonical unresolved record set with explicit record count. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_unresolved_metadata(std::span<const unresolved_item> values);
	/** @brief Decode canonical unresolved records without losing detail. */
	[[nodiscard]] result<std::vector<unresolved_item>>
	decode_unresolved_metadata(std::span<const std::byte> control);

	/** @brief Encode/decode a canonical evidence record set, including every summary. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_evidence_metadata(std::span<const evidence_item> values);
	/** @brief Decode canonical evidence records including every summary. */
	[[nodiscard]] result<std::vector<evidence_item>>
	decode_evidence_metadata(std::span<const std::byte> control);

	/** @brief Encode/decode exact deterministic CBOR metadata for task_complete. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_task_complete_metadata(const task_complete_metadata& value);
	/** @brief Decode and validate exact task_complete CBOR metadata. */
	[[nodiscard]] result<task_complete_metadata>
	decode_task_complete_metadata(std::span<const std::byte> control);

	/** @brief Encode/decode exact deterministic CBOR metadata for task_failed. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_task_failed_metadata(const task_failed_metadata& value);
	/** @brief Decode and validate exact task_failed CBOR metadata. */
	[[nodiscard]] result<task_failed_metadata>
	decode_task_failed_metadata(std::span<const std::byte> control);

	/** @brief Encode/decode exact deterministic CBOR metadata for schema_negotiate. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_schema_negotiate_metadata(const schema_negotiate_metadata& value);
	/** @brief Decode and validate exact schema_negotiate CBOR metadata. */
	[[nodiscard]] result<schema_negotiate_metadata>
	decode_schema_negotiate_metadata(std::span<const std::byte> control);

	/** @brief Encode/decode exact deterministic CBOR metadata for open_task. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_open_task_metadata(const open_task_metadata& value);
	/** @brief Decode and validate exact open_task CBOR metadata. */
	[[nodiscard]] result<open_task_metadata>
	decode_open_task_metadata(std::span<const std::byte> control);

	/** @brief Encode/decode exact deterministic CBOR metadata for credit. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_credit_metadata(const credit_metadata& value);
	/** @brief Decode and validate exact credit CBOR metadata. */
	[[nodiscard]] result<credit_metadata>
	decode_credit_metadata(std::span<const std::byte> control);

	/** @brief Encode/decode exact deterministic CBOR metadata for close. */
	[[nodiscard]] result<std::vector<std::byte>> encode_close_metadata(const close_metadata& value);
	/** @brief Decode and validate exact close CBOR metadata. */
	[[nodiscard]] result<close_metadata> decode_close_metadata(std::span<const std::byte> control);

	/** @brief Encode the canonical five-frame host task transcript. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_host_transcript(const host_transcript_request& request);
	/** @brief Validate exact host direction, state, sequence, bindings, and payload digest. */
	[[nodiscard]] result<validated_host_transcript>
	validate_host_transcript(std::span<const frame> frames,
							 const host_transcript_expectation& expectation);

	/** @brief Exact provider identity, relation offer, interpretation, and stage authority. */
	struct provider_session
	{
		/** @brief Namespaced provider implementation identity. */
		std::string provider_id;
		/** @brief Exact accepted provider semantic version. */
		semantic_version provider_version;
		/** @brief Exact semantic contract digest implemented by the callback. */
		std::string provider_semantic_contract_digest;
		/** @brief Canonical exact relation descriptors the session may offer. */
		std::vector<relation_descriptor> offered_outputs;
		/** @brief Canonical exact relation descriptors required as input. */
		std::vector<relation_descriptor> required_relations;
		/** @brief Canonical interpretation domains authorized by the session. */
		std::vector<std::string> interpretation_domains;
		/** @brief Stable input-stage identifier. */
		std::string input_stage;
		/** @brief Stable output-stage identifier. */
		std::string output_stage;
		/** @brief Validate identity, descriptor sets, domains, and stages. */
		[[nodiscard]] result<void> validate() const;
	};

	/** @brief Exact portable provider task; all values and identity inputs are detached. */
	struct task
	{
		/** @brief Content identity derived from canonical_projection(). */
		std::string task_id;
		/** @brief Exact provider session authorized for this task. */
		provider_session session;
		/** @brief Validated exact project input catalog. */
		project_catalog project;
		/** @brief Canonical requested output descriptor subset. */
		std::vector<relation_descriptor> outputs;
		/** @brief Nonempty canonical condition authority. */
		std::string condition;
		/** @brief Exact authorized interpretation domain. */
		std::string interpretation;
		/** @brief Canonical dependency groups authorized for output. */
		std::vector<std::string> dependency_groups;
		/** @brief Canonicalize task sets and derive its content identity. */
		[[nodiscard]] static result<task> make(provider_session session_value,
											   project_catalog project_value,
											   std::vector<relation_descriptor> output_values,
											   std::string condition_value,
											   std::string interpretation_value,
											   std::vector<std::string> dependency_group_values = {
												   "default"});
		/** @brief Encode every semantic task and session field in exact order. */
		[[nodiscard]] result<std::vector<std::byte>> canonical_projection() const;
		/** @brief Revalidate the complete task and recompute task_id. */
		[[nodiscard]] result<void> validate() const;
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

	/**
	 * @brief Move-only high-level relation stream; providers never construct protocol frames.
	 * @details Open state is registered with the callback context. Destruction never seals a batch;
	 * an abandoned, duplicate, interleaved, or send-failed batch makes context validation fail.
	 */
	class relation_sink
	{
	  public:
		relation_sink(const relation_sink&) = delete;
		relation_sink& operator=(const relation_sink&) = delete;
		relation_sink(relation_sink&&) noexcept = default;
		relation_sink& operator=(relation_sink&&) = delete;
		~relation_sink();
		[[nodiscard]] result<void>
		begin(std::string dependency_group, std::string atomic_output_group, std::string batch_id);
		[[nodiscard]] result<void> push(const detached_row& row);
		[[nodiscard]] result<void> end();
		/** @brief Row count of the active batch, or the most recently ended/failed batch. */
		[[nodiscard]] std::uint64_t row_count() const noexcept;

	  private:
		relation_sink(protocol_writer& writer,
					  relation_descriptor descriptor,
					  std::string task_id,
					  std::uint64_t& total_rows,
					  std::uint64_t maximum_rows,
					  bool authorized,
					  std::span<const std::string> dependency_groups,
					  std::shared_ptr<detail::relation_sink_registry> registry,
					  std::uint64_t sink_id);
		[[nodiscard]] result<void> flush_chunk();
		protocol_writer* writer_{};
		relation_descriptor descriptor_;
		std::string task_id_;
		std::string dependency_group_;
		std::string atomic_output_group_;
		std::string batch_id_;
		std::vector<detached_row> pending_rows_;
		std::vector<batch_column_summary> column_summaries_;
		std::vector<std::string> ordered_chunk_digests_;
		std::uint64_t row_count_{};
		std::uint64_t emitted_rows_{};
		std::uint64_t* total_rows_{};
		std::uint64_t maximum_rows_{};
		std::vector<std::string> dependency_groups_;
		std::shared_ptr<detail::relation_sink_registry> registry_;
		std::uint64_t sink_id_{};
		bool authorized_{};
		bool open_{};
		bool poisoned_{};
		friend class context;
	};

	/** @brief Provider callback context with streaming and partiality helpers. */
	class context
	{
	  public:
		context(protocol_writer& writer,
				execution_context execution,
				std::string task_id = {},
				std::span<const relation_descriptor> outputs = {},
				std::span<const std::string> dependency_groups = {});
		[[nodiscard]] relation_sink relation(relation_descriptor descriptor);
		[[nodiscard]] coverage_builder& coverage() noexcept;
		[[nodiscard]] unresolved_builder& unresolved() noexcept;
		[[nodiscard]] evidence_builder& evidence() noexcept;
		[[nodiscard]] bool stop_requested() const noexcept;
		/** @brief Reject unauthorized output and any open, abandoned, or invalid batch state. */
		[[nodiscard]] result<void> validate() const;

	  private:
		protocol_writer* writer_{};
		execution_context execution_;
		std::string task_id_;
		std::vector<relation_descriptor> outputs_;
		std::vector<std::string> dependency_groups_;
		std::shared_ptr<detail::relation_sink_registry> sink_registry_;
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
		/** @brief Return the exact semantic contract digest bound to accepted tasks. */
		[[nodiscard]] virtual std::string_view semantic_contract_digest() const noexcept = 0;
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
	/** @brief Test exact membership in the closed sandbox assurance enum. */
	[[nodiscard]] constexpr bool is_valid(const sandbox_assurance value) noexcept
	{
		return value >= sandbox_assurance::none && value <= sandbox_assurance::certified;
	}

	/** @brief Immutable built-in sandbox policy projected into an exact semantic digest. */
	struct sandbox_policy
	{
		std::string id;
		std::vector<std::string> mechanisms;
		bool deny_network{};
		bool zero_core_dump{};
		bool zero_locked_memory{};
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string canonical_form() const;
		[[nodiscard]] std::string policy_digest() const;
	};

	/** @brief Return value-owned built-in policies in canonical policy-ID order. */
	[[nodiscard]] std::vector<sandbox_policy> builtin_sandbox_policies();

	/** @brief Resolve one exact built-in policy digest or fail closed. */
	[[nodiscard]] result<sandbox_policy> resolve_sandbox_policy(std::string_view policy_digest);

	/**
	 * @brief Recompute evidence identity from the measured executable, policy, plan, and result.
	 * @return The digest, or `provider.sandbox-report-invalid` when `achieved` or the measured
	 * executable digest is invalid.
	 */
	[[nodiscard]] result<std::string>
	sandbox_evidence_digest(const sandbox_policy& policy,
							const execution_budget& budget,
							sandbox_assurance achieved,
							std::span<const std::string> applied_mechanisms,
							std::string_view measured_executable_digest);

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
	/** @brief Test exact membership in the closed discovery source enum. */
	[[nodiscard]] constexpr bool is_valid(const discovery_source value) noexcept
	{
		return value >= discovery_source::explicit_path &&
			value <= discovery_source::system_registry;
	}

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
	/** @brief Test exact membership in the closed fallback direction enum. */
	[[nodiscard]] constexpr bool is_valid(const fallback_direction value) noexcept
	{
		return value >= fallback_direction::upgrade &&
			value <= fallback_direction::same_version_rebuild;
	}

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
		/** @brief Full canonical manifest/executable/sandbox/certification identity digest. */
		std::string candidate_digest;
		bool selected{};
		std::string reason;
		[[nodiscard]] bool operator==(const provider_candidate_decision&) const = default;
	};

	/** @brief Immutable validated selection token plus complete rejection/fallback evidence. */
	class provider_selection
	{
	  public:
		provider_selection() = default;
		/** @brief Return the immutable candidate authorized by this token. */
		[[nodiscard]] const provider_candidate& selected_candidate() const noexcept;
		/** @brief Return the immutable request that defined selection authority. */
		[[nodiscard]] const provider_selection_request& authority_request() const noexcept;
		/** @brief Return complete immutable selected and rejected candidate evidence. */
		[[nodiscard]] std::span<const provider_candidate_decision> decisions() const noexcept;
		/** @brief Whether the selected candidate used the explicit fallback policy. */
		[[nodiscard]] bool fallback_used() const noexcept;
		/** @brief Return the exact fallback policy binding, if one was requested. */
		[[nodiscard]] const std::optional<std::string>& fallback_policy_digest() const noexcept;
		/** @brief Revalidate selection authority, exact decision, and fallback policy binding. */
		[[nodiscard]] result<void> validate() const;
		/** @brief Render immutable selection evidence in canonical order. */
		[[nodiscard]] std::string canonical_form() const;

	  private:
		provider_candidate candidate_;
		provider_selection_request request_;
		std::vector<provider_candidate_decision> decisions_;
		bool fallback_used_{};
		std::optional<std::string> fallback_policy_digest_;
		bool validated_{};

		provider_selection(provider_candidate candidate,
						   provider_selection_request request,
						   std::vector<provider_candidate_decision> decisions,
						   bool fallback_used,
						   std::optional<std::string> fallback_policy_digest);
		friend result<provider_selection> select_provider(const provider_selection_request&,
														  std::span<const provider_candidate>);
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
	/** @brief Test exact membership in the closed process status enum. */
	[[nodiscard]] constexpr bool is_valid(const process_status value) noexcept
	{
		return value >= process_status::exited && value <= process_status::launch_failed;
	}

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
		/** @brief Digest of the immutable executable image measured by the process port. */
		std::string measured_executable_digest;
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
		std::string measured_executable_digest;
		sandbox_report sandbox;
		std::vector<frame> frames;
		std::vector<unresolved_item> diagnostics;
		int exit_code{};
		int termination_signal{};
		[[nodiscard]] bool succeeded() const noexcept;
		[[nodiscard]] std::string canonical_form() const;
		[[nodiscard]] std::string semantic_digest() const;

	  private:
		bool validated_success_{};
		friend class process_provider_runtime;
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

	/**
	 * @brief Generate CMake, runtime-valid 1.0.0 manifest, source, test, and README files.
	 *
	 * The generated manifest is independently validated before any files are returned.
	 */
	[[nodiscard]] result<std::vector<scaffold_file>> make_scaffold(const scaffold_options& options);

	/** @brief Run one task through the protocol server; framing and terminal messages are owned. */
	[[nodiscard]] result<void> run_worker(portable_provider& provider,
										  const task& task,
										  protocol_writer& writer,
										  execution_context execution = {});
} // namespace cxxlens::sdk::provider
