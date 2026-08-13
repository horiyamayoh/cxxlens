#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_admission_error.hpp"
#include "materialization_io.hpp"
#include "materialization_json.hpp"
#include "provider_task_v3.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::string_view materialization_request_schema_v2 =
		"cxxlens.clang22-materialization-request.v2";
	inline constexpr std::string_view materialization_request_version_v2_1 = "2.1.0";
	inline constexpr std::size_t maximum_materialization_request_schema_capture_bytes =
		materialization_request_schema_v2.size() + 1U;
	inline constexpr std::size_t maximum_materialization_request_version_capture_bytes =
		materialization_request_version_v2_1.size() + 1U;
	/** Selected-schema semantic JSON windows; raw spelling is canonicalized while replaying. */
	inline constexpr std::size_t maximum_materialization_global_request_window_bytes =
		64U * 1024U * 1024U;
	inline constexpr std::size_t maximum_materialization_task_metadata_window_bytes =
		64U * 1024U * 1024U;

	/** Bounded pass-one limits for the accepted v2.1 streaming request grammar. */
	struct materialization_request_scan_limits
	{
		std::size_t chunk_bytes{default_stream_chunk_bytes};
		std::size_t maximum_depth{64U};
		std::size_t maximum_members_per_object{4096U};
		/** Lexical safety limit only; selected-schema maxItems is enforced by its owning path. */
		std::size_t maximum_elements_per_array{static_cast<std::size_t>(maximum_raw_request_bytes)};
		std::size_t maximum_member_name_utf8_bytes{256U};
		std::size_t maximum_string_utf8_bytes{static_cast<std::size_t>(maximum_raw_request_bytes)};
	};

	/** Append-only pass-one sink for the compact, spool-backed task span index. */
	class materialization_request_task_span_sink
	{
	  public:
		virtual ~materialization_request_task_span_sink() = default;
		[[nodiscard]] virtual sdk::result<void>
		append(std::uint64_t value_offset,
			   std::uint64_t value_size_bytes,
			   std::optional<std::uint64_t> source_content_offset,
			   std::optional<std::uint64_t> source_content_size_bytes) = 0;
		[[nodiscard]] virtual sdk::result<void> seal() = 0;
	};

	struct materialization_request_task_span
	{
		std::uint64_t value_offset{};
		std::uint64_t value_size_bytes{};
		std::optional<std::uint64_t> source_content_offset;
		std::optional<std::uint64_t> source_content_size_bytes;

		[[nodiscard]] bool operator==(const materialization_request_task_span&) const = default;
	};

	/** Sealed fixed-record task index whose entries remain outside the retained task window. */
	class materialization_request_task_index final : public materialization_request_task_span_sink
	{
	  public:
		materialization_request_task_index(const materialization_request_task_index&) = delete;
		materialization_request_task_index&
		operator=(const materialization_request_task_index&) = delete;
		materialization_request_task_index(materialization_request_task_index&&) noexcept = delete;
		materialization_request_task_index&
		operator=(materialization_request_task_index&&) noexcept = delete;
		~materialization_request_task_index() override = default;

		[[nodiscard]] std::uint64_t task_count() const noexcept;
		[[nodiscard]] bool sealed() const noexcept;
		[[nodiscard]] sdk::result<materialization_request_task_span> at(std::uint64_t index);

	  private:
		materialization_request_task_index(std::unique_ptr<materialization_replayable_spool> spool,
										   std::uint64_t request_size_bytes);
		[[nodiscard]] sdk::result<void>
		append(std::uint64_t value_offset,
			   std::uint64_t value_size_bytes,
			   std::optional<std::uint64_t> source_content_offset,
			   std::optional<std::uint64_t> source_content_size_bytes) override;
		[[nodiscard]] sdk::result<void> seal() override;

		std::unique_ptr<materialization_replayable_spool> spool_;
		std::uint64_t request_size_bytes_{};
		std::uint64_t task_count_{};
		bool sealed_{};

		friend sdk::result<std::unique_ptr<materialization_request_task_index>>
		make_materialization_request_task_index(std::uint64_t);
		friend sdk::result<std::unique_ptr<materialization_request_task_index>>
		make_materialization_request_task_index(std::unique_ptr<materialization_replayable_spool>,
												std::uint64_t);
	};

	/** Create an unsealed private task index for exactly one bounded raw request. */
	[[nodiscard]] sdk::result<std::unique_ptr<materialization_request_task_index>>
	make_materialization_request_task_index(std::uint64_t request_size_bytes);

	/** Dependency-injected task-index storage used by exact create/append/seal/read fault tests. */
	[[nodiscard]] sdk::result<std::unique_ptr<materialization_request_task_index>>
	make_materialization_request_task_index(
		std::unique_ptr<materialization_replayable_spool> storage,
		std::uint64_t request_size_bytes);

	/** Exact envelope discovered only after the complete strict JSON document has been scanned. */
	class materialization_request_envelope
	{
	  public:
		struct member_span
		{
			std::string name;
			std::uint64_t value_offset{};
			std::uint64_t value_size_bytes{};

			[[nodiscard]] bool operator==(const member_span&) const = default;
		};

		[[nodiscard]] const std::string& schema() const noexcept;
		[[nodiscard]] const std::string& request_version() const noexcept;
		[[nodiscard]] std::uint64_t scanned_size_bytes() const noexcept;
		/** Exact top-level raw value spans; names are decoded and duplicate-free. */
		[[nodiscard]] const std::vector<member_span>& members() const noexcept;
		materialization_request_envelope(std::string schema,
										 std::string request_version,
										 std::uint64_t scanned_size_bytes,
										 std::vector<member_span> members);

		[[nodiscard]] bool operator==(const materialization_request_envelope&) const = default;

	  private:
		std::string schema_;
		std::string request_version_;
		std::uint64_t scanned_size_bytes_{};
		std::vector<member_span> members_;

		friend sdk::result<materialization_request_envelope>
		scan_materialization_request_envelope(materialization_replayable_spool&,
											  materialization_request_scan_limits,
											  materialization_request_task_span_sink*);
	};

	/**
	 * Pass one over an immutable request spool without constructing a request DOM.
	 *
	 * The scanner validates strict UTF-8 JSON, integer-valued JSON numbers, maximum depth,
	 * duplicate decoded member names at every object depth, exactly one top-level object, and the
	 * exact v2 schema/version envelope. It never retains value strings other than the two envelope
	 * values and replays correctly across arbitrary positive short reads.
	 */
	[[nodiscard]] sdk::result<materialization_request_envelope>
	scan_materialization_request_envelope(
		materialization_replayable_spool& spool,
		materialization_request_scan_limits limits = {},
		materialization_request_task_span_sink* task_index = nullptr);

	/**
	 * Replay only the bounded global authority into a DOM, replacing the task array with `[]`.
	 * The exact root member set is checked before allocation and no task occurrence bytes are
	 * copied.
	 */
	[[nodiscard]] sdk::result<json_document> replay_materialization_request_globals(
		materialization_replayable_spool& spool,
		const materialization_request_envelope& envelope,
		std::size_t maximum_window_bytes = maximum_materialization_global_request_window_bytes,
		std::string_view phase = "request-schema");

	/** Replay one task object while replacing its bulk source token with the empty JSON string. */
	[[nodiscard]] sdk::result<json_document> replay_materialization_task_metadata(
		materialization_replayable_spool& spool,
		const materialization_request_task_span& task,
		std::size_t maximum_window_bytes = maximum_materialization_task_metadata_window_bytes,
		std::string_view phase = "request-schema");

	/**
	 * Establish exact JSON-value equality for two selected-schema task occurrences.
	 *
	 * Metadata is replayed through the bounded task window and the indexed bulk string is compared
	 * incrementally after JSON escape decoding. This is the collision fallback for the compact
	 * task uniqueness index; a digest match alone is never equality authority.
	 */
	[[nodiscard]] sdk::result<bool>
	exact_materialization_task_json_equal(materialization_replayable_spool& spool,
										  const materialization_request_task_span& left,
										  const materialization_request_task_span& right,
										  materialization_replayable_spool& metadata_comparison);

	/** Decode the indexed JSON/base64 source token directly into one fresh private source spool. */
	[[nodiscard]] sdk::result<clang22_task_source_receipt>
	decode_materialization_task_source(materialization_replayable_spool& request,
									   const materialization_request_task_span& task,
									   clang22_task_source_spool& source,
									   std::string_view phase = "request-schema");
} // namespace cxxlens::detail::clang22::materialization
