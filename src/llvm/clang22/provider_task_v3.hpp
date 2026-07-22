#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

namespace cxxlens::detail::clang22
{
	/** Immutable decoded-source authority retained beside a private source spool. */
	struct clang22_task_source_receipt
	{
		std::uint64_t size_bytes{};
		std::string content_digest;
		std::string line_index_id;

		[[nodiscard]] bool operator==(const clang22_task_source_receipt&) const = default;
	};

	/** Exact installed Clang 22 task.v3 input, independent from native parser state. */
	struct clang22_task_input
	{
		/** Exact global project-catalog authority carried by task.v3. */
		sdk::project_catalog project_catalog;
		/** Catalog-local input identity selected for this translation unit. */
		std::string selected_catalog_compile_unit;
		/** Independently derived build.compile_unit relation identity. */
		std::string compile_unit;
		std::string project;
		std::string variant;
		std::string toolchain_context;
		std::string toolchain_digest;
		struct toolchain_fields
		{
			std::string family;
			std::string exact_version;
			std::string target_triple;
			std::string builtin_headers_digest;
			std::optional<std::string> sysroot;
			std::string abi_digest;
			std::string plugin_spec_digest;
		} toolchain;
		struct variant_fields
		{
			std::string language;
			std::string language_standard;
			std::string target_triple;
			std::string predefined_macros_digest;
			std::string include_search_digest;
			std::string semantic_flags_digest;
		} variant_authority;
		std::string normalized_invocation_digest;
		std::string environment_digest;
		std::string language;
		std::string working_directory;
		std::string condition_universe;
		std::string condition;
		std::string interpretation;
		std::string source_snapshot;
		std::string file;
		std::string logical_path;
		std::string source_content_digest;
		/** Unique canonical RFC 4648 spelling derived from the sealed source bytes. */
		std::string source_content_base64;
		std::uint64_t source_size_bytes{};
		std::string source_encoding;
		std::string line_index;
		bool source_read_only{};
		std::string source;
		std::vector<std::string> arguments;
		std::vector<std::string> requested_descriptors;
		std::vector<std::string> dependency_groups;
		sdk::provider::execution_budget budget;
		struct sandbox_fields
		{
			std::string minimum;
			std::string policy_digest;
		} sandbox;

		[[nodiscard]] sdk::result<void> validate() const;
		/** Validate all task authority against an independently sealed private source spool. */
		[[nodiscard]] sdk::result<void>
		validate_with_source_receipt(const clang22_task_source_receipt& source_receipt) const;
		/** Validate against one external immutable catalog owner without copying its census. */
		[[nodiscard]] sdk::result<void>
		validate_with_catalog(const sdk::project_catalog& catalog,
							  const clang22_task_source_receipt& source_receipt) const;

	  private:
		[[nodiscard]] sdk::result<void>
		validate_impl(const sdk::project_catalog& catalog,
					  const clang22_task_source_receipt* source_receipt) const;
	};

	/** Cursor-independent decoded-source input for the source-private streaming task.v3 encoder. */
	class clang22_task_source_replay
	{
	  public:
		virtual ~clang22_task_source_replay() = default;
		[[nodiscard]] virtual sdk::result<std::size_t>
		read_at(std::uint64_t offset, std::span<std::byte> destination) = 0;
		[[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;
		[[nodiscard]] virtual bool sealed() const noexcept = 0;
		[[nodiscard]] virtual const clang22_task_source_receipt& receipt() const noexcept = 0;
	};

	/** Append-only decoded-source spool which becomes replayable only after one successful seal. */
	class clang22_task_source_spool : public clang22_task_source_replay
	{
	  public:
		~clang22_task_source_spool() override = default;
		[[nodiscard]] virtual sdk::result<void> append(std::span<const std::byte> bytes) = 0;
		[[nodiscard]] virtual sdk::result<clang22_task_source_receipt> seal() = 0;
	};

	/** Sealed cursor-independent replay of the exact logical task.v3 bytes. */
	class clang22_task_input_replay
	{
	  public:
		virtual ~clang22_task_input_replay() = default;
		[[nodiscard]] virtual sdk::result<std::size_t>
		read_at(std::uint64_t offset, std::span<std::byte> destination) = 0;
		[[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;
		[[nodiscard]] virtual bool sealed() const noexcept = 0;
	};

	/** All-bytes-or-error sink for one canonical task.v3 private spool. */
	class clang22_task_input_sink
	{
	  public:
		virtual ~clang22_task_input_sink() = default;
		[[nodiscard]] virtual sdk::result<void> append(std::span<const std::byte> bytes) = 0;
	};

	struct clang22_task_v3_stream_receipt
	{
		std::uint64_t source_size_bytes{};
		std::uint64_t canonical_base64_bytes{};
		std::uint64_t task_input_bytes{};

		[[nodiscard]] bool operator==(const clang22_task_v3_stream_receipt&) const = default;
	};

	/** Metadata decoded without retaining logical task bytes or source/base64 bytes in memory. */
	struct clang22_task_v3_stream_decoded
	{
		clang22_task_input input;
		clang22_task_source_receipt source;
		std::uint64_t canonical_base64_bytes{};
		std::uint64_t task_input_bytes{};
	};

	[[nodiscard]] sdk::result<std::vector<std::byte>>
	encode_task_input(const clang22_task_input& input);
	/**
	 * Canonically encode task.v3 without retaining decoded source, base64, or logical input bytes.
	 * `input.source` and `input.source_content_base64` must be empty; source authority is replayed
	 * exactly once from `source` and canonically base64 encoded into the task projection.
	 */
	[[nodiscard]] sdk::result<clang22_task_v3_stream_receipt>
	encode_task_input_streaming(const clang22_task_input& input,
								clang22_task_source_replay& source,
								clang22_task_input_sink& output);
	/** Source-private catalog-reference form used by bounded request admission. */
	[[nodiscard]] sdk::result<clang22_task_v3_stream_receipt>
	encode_task_input_streaming(const clang22_task_input& input,
								const sdk::project_catalog& catalog,
								clang22_task_source_replay& source,
								clang22_task_input_sink& output);
	[[nodiscard]] sdk::result<clang22_task_input>
	decode_task_input(std::span<const std::byte> input);
	/**
	 * Decode one sealed task.v3 replay without materializing its complete logical byte vector.
	 * The unique canonical `source.content_base64` spelling is validated incrementally and decoded
	 * directly into `source`, which is sealed before a successful result is returned.
	 */
	[[nodiscard]] sdk::result<clang22_task_v3_stream_decoded>
	decode_task_input_streaming(clang22_task_input_replay& input,
								clang22_task_source_spool& source);
	/** @brief Derive the exact generic condition-ref from the ordered request pair. */
	[[nodiscard]] sdk::result<std::string>
	provider_condition_ref_id(const clang22_task_input& input);
	[[nodiscard]] sdk::result<std::string>
	provider_condition_ref_id(const clang22_task_input& input,
							  const clang22_task_source_receipt& source);
	/** @brief Reconstruct the shared portable task from task.v3 and validated worker authority. */
	[[nodiscard]] sdk::result<sdk::provider::task>
	reconstruct_provider_task(const clang22_task_input& input,
							  std::vector<sdk::relation_descriptor> exact_outputs,
							  std::string provider_semantic_contract_digest);
	[[nodiscard]] sdk::result<sdk::provider::task>
	reconstruct_provider_task(const clang22_task_input& input,
							  const clang22_task_source_receipt& source,
							  std::vector<sdk::relation_descriptor> exact_outputs,
							  std::string provider_semantic_contract_digest);
	/** Derive the exact portable provider task ID from one external immutable catalog owner. */
	[[nodiscard]] sdk::result<std::string>
	reconstruct_provider_task_id(const clang22_task_input& input,
								 const sdk::project_catalog& catalog,
								 const clang22_task_source_receipt& source,
								 std::vector<sdk::relation_descriptor> exact_outputs,
								 std::string provider_semantic_contract_digest);
} // namespace cxxlens::detail::clang22
