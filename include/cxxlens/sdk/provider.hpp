#pragma once

/** @file provider.hpp @brief Portable provider authoring without manual wire framing. */

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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
	enum class message_type : std::uint8_t
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

	/** @brief Negotiated protocol frame limits. */
	struct protocol_limits
	{
		std::uint32_t max_control_bytes{65536U};
		std::uint64_t max_payload_bytes{16777216U};
	};

	/** @brief Decoded and checksum-validated protocol frame. */
	struct frame
	{
		message_type type{message_type::hello};
		std::uint64_t stream_id{};
		std::uint64_t sequence{};
		std::vector<std::byte> control;
		std::vector<std::byte> payload;
	};

	/** @brief Encode one 104-byte-header protocol frame with independent SHA-256 checksums. */
	[[nodiscard]] result<std::vector<std::byte>> encode_frame(const frame& value,
															  protocol_limits limits = {});
	/** @brief Decode, bound-check, and verify exactly one protocol frame. */
	[[nodiscard]] result<frame> decode_frame(std::span<const std::byte> input,
											 protocol_limits limits = {});

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
										std::span<const std::byte> payload = {});
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
