#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/store.hpp>

#include "sqlite_payload_streaming_internal.hpp"

namespace cxxlens::sdk
{
	/** Source-private complete snapshot graph shared by Store and its canonical codec. */
	struct snapshot_handle::data
	{
		snapshot_manifest semantic_manifest;
		publication_record publication_record_value;
		std::map<std::string, relation_descriptor, std::less<>> descriptors;
		std::map<std::string, std::vector<detached_row>, std::less<>> rows;
		std::map<std::string, std::vector<snapshot_claim_annotation>, std::less<>> annotations;
		std::vector<snapshot_query_coverage> coverage;
		std::vector<snapshot_partition_binding> partition_bindings;
		std::map<std::string, partition_draft, std::less<>> partition_envelopes;
		std::vector<closure_certificate> closure_certificates;
		std::vector<std::string> claim_contents;
		std::vector<unresolved_reference> unresolved;
		std::string physical_backend;
		bool query_annotations_available{};
		/** Legacy Store handle lifetime pin; never a materializer/session authority token. */
		std::shared_ptr<const std::uint64_t> generation_pin;
	};

	namespace detail
	{
		inline constexpr std::uint64_t snapshot_store_v5_maximum_payload_bytes =
			512U * 1024U * 1024U;

		enum class snapshot_payload_schema : std::uint8_t
		{
			v1 = 1U,
			v2 = 2U,
			v3 = 3U,
			v4 = 4U,
			v5 = 5U,
		};

		[[nodiscard]] result<std::vector<std::byte>>
		encode_snapshot(const snapshot_handle::data& value);

#if defined(CXXLENS_STORE_FAULT_TEST_SUPPORT)
		[[nodiscard]] std::optional<snapshot_payload_schema>
		payload_schema_from_number(std::uint8_t number) noexcept;
		[[nodiscard]] result<std::vector<std::byte>>
		encode_snapshot(const snapshot_handle::data& value, snapshot_payload_schema payload_schema);
#endif

		[[nodiscard]] result<void> encode_snapshot(const snapshot_handle::data& value,
												   sqlite_bounded_byte_sink& sink);

		/**
		 * Decode exactly one replayable payload.
		 *
		 * A size above the hard v5 bound is rejected before opening the source. Size zero preserves
		 * the existing Store contract: one empty pass is opened and classified as a corrupt payload
		 * format, rather than being reclassified as a resource-limit failure.
		 */
		[[nodiscard]] result<std::shared_ptr<snapshot_handle::data>>
		decode_snapshot(const sqlite_replayable_byte_source& source,
						std::uint64_t expected_size,
						const relation_engine& engine);

		[[nodiscard]] std::string canonical_export_of(const snapshot_handle::data& value);
		[[nodiscard]] std::vector<std::byte>
		semantic_projection_bytes(const snapshot_handle::data& value);
		[[nodiscard]] std::vector<std::byte>
		annotation_projection(const snapshot_claim_annotation& value);
		void sort_semantic_projections(snapshot_handle::data& value);
		[[nodiscard]] result<void> validate_semantic_graph(snapshot_handle::data& value,
														   const relation_engine& engine);

		/** Return the generation field offset after validating the complete streamed header. */
		[[nodiscard]] result<std::size_t>
		payload_generation_offset(const sqlite_replayable_byte_source& source,
								  std::uint64_t expected_size,
								  std::uint64_t expected_generation);

#if defined(CXXLENS_STORE_FAULT_TEST_SUPPORT)
		/** Return the byte offset of one v5 semantic-version component in a complete payload. */
		[[nodiscard]] result<std::size_t>
		snapshot_version_component_offset(std::span<const std::byte> payload,
										  std::size_t component_index);
#endif

		/** Exact bounded observation of one sealed canonical-v5 staging object. */
		struct snapshot_store_v5_staging_observation
		{
			std::uint64_t byte_count{};
			std::string payload_sha256;
			std::string snapshot_id;
			std::string publication_id;
			std::string series_id;
			std::uint64_t sequence{};
			std::uint64_t physical_generation{};

			[[nodiscard]] bool
			operator==(const snapshot_store_v5_staging_observation&) const = default;
		};

		/**
		 * One owned prepublication staging object.
		 *
		 * Before a successful non-null `seal()`, every failed stream attempt invokes the common
		 * one-shot `discard()` ledger exactly once. Implementations provide only the physical
		 * discard operation; they cannot observe a replay through this interface. After `seal()`,
		 * the returned replay source owns cleanup of the immutable staged object.
		 */
		class snapshot_store_v5_staging_sink : public sqlite_replayable_byte_sink
		{
		  public:
			[[nodiscard]] result<void> discard();

		  protected:
			[[nodiscard]] virtual result<void> discard_staging() = 0;

		  private:
			bool discard_attempted_{};
		};

		class snapshot_store_v5_staging_binding;

		/** One forward-only reread which authenticates EOF, byte count, and SHA-256 at finish. */
		class snapshot_store_v5_authenticated_cursor final
		{
		  public:
			snapshot_store_v5_authenticated_cursor(const snapshot_store_v5_authenticated_cursor&) =
				delete;
			snapshot_store_v5_authenticated_cursor&
			operator=(const snapshot_store_v5_authenticated_cursor&) = delete;
			snapshot_store_v5_authenticated_cursor(
				snapshot_store_v5_authenticated_cursor&&) noexcept;
			snapshot_store_v5_authenticated_cursor&
			operator=(snapshot_store_v5_authenticated_cursor&&) noexcept;
			~snapshot_store_v5_authenticated_cursor();

			[[nodiscard]] result<std::size_t> read(std::span<std::byte> output);
			[[nodiscard]] result<snapshot_store_v5_staging_observation> finish();

		  private:
			struct state;
			explicit snapshot_store_v5_authenticated_cursor(std::unique_ptr<state>);
			std::unique_ptr<state> state_;
			friend class snapshot_store_v5_staging_binding;
		};

		class snapshot_store_v5_measurement;

		/** Sealed exact bytes plus their immutable typed graph identity. */
		class snapshot_store_v5_staging_binding final
		{
		  public:
			snapshot_store_v5_staging_binding(const snapshot_store_v5_staging_binding&) = delete;
			snapshot_store_v5_staging_binding&
			operator=(const snapshot_store_v5_staging_binding&) = delete;
			snapshot_store_v5_staging_binding(snapshot_store_v5_staging_binding&&) noexcept;
			snapshot_store_v5_staging_binding&
			operator=(snapshot_store_v5_staging_binding&&) noexcept;
			~snapshot_store_v5_staging_binding();

			[[nodiscard]] const snapshot_store_v5_staging_observation& observation() const noexcept;
			[[nodiscard]] result<snapshot_store_v5_authenticated_cursor> take_cursor() &&;

		  private:
			struct state;
			explicit snapshot_store_v5_staging_binding(std::unique_ptr<state>);
			std::unique_ptr<state> state_;
			friend class snapshot_store_v5_measurement;
		};

		class snapshot_store_v5_graph_binding;

		/** Bound checked before any staging sink is accepted or written. */
		class snapshot_store_v5_measurement final
		{
		  public:
			snapshot_store_v5_measurement(const snapshot_store_v5_measurement&) = delete;
			snapshot_store_v5_measurement& operator=(const snapshot_store_v5_measurement&) = delete;
			snapshot_store_v5_measurement(snapshot_store_v5_measurement&&) noexcept;
			snapshot_store_v5_measurement& operator=(snapshot_store_v5_measurement&&) noexcept;
			~snapshot_store_v5_measurement();

			[[nodiscard]] std::uint64_t byte_count() const noexcept;
			[[nodiscard]] std::uint64_t maximum_bytes() const noexcept;
			[[nodiscard]] result<snapshot_store_v5_staging_binding>
			stream(std::unique_ptr<snapshot_store_v5_staging_sink> sink) &&;

		  private:
			struct state;
			explicit snapshot_store_v5_measurement(std::unique_ptr<state>);
			std::unique_ptr<state> state_;
			friend class snapshot_store_v5_graph_binding;
		};

		/**
		 * Move-only reference binding for the existing Store full graph.
		 *
		 * This adapter exists to prove exact parity with the current Store implementation. The
		 * production materializer supplies its own sealed typed cursor authority and must not build
		 * a second request-sized `snapshot_handle::data` graph.
		 */
		class snapshot_store_v5_graph_binding final
		{
		  public:
			snapshot_store_v5_graph_binding(const snapshot_store_v5_graph_binding&) = delete;
			snapshot_store_v5_graph_binding&
			operator=(const snapshot_store_v5_graph_binding&) = delete;
			snapshot_store_v5_graph_binding(snapshot_store_v5_graph_binding&&) noexcept;
			snapshot_store_v5_graph_binding& operator=(snapshot_store_v5_graph_binding&&) noexcept;
			~snapshot_store_v5_graph_binding();

			[[nodiscard]] static result<snapshot_store_v5_graph_binding>
			seal_reference(std::unique_ptr<snapshot_handle::data> graph,
						   const relation_engine& engine);
			[[nodiscard]] result<snapshot_store_v5_measurement>
			measure(std::uint64_t maximum_bytes) &&;

		  private:
			struct state;
			explicit snapshot_store_v5_graph_binding(std::unique_ptr<state>);
			std::unique_ptr<state> state_;
		};

		[[nodiscard]] result<snapshot_store_v5_graph_binding>
		seal_snapshot_store_v5_reference_graph(std::unique_ptr<snapshot_handle::data> graph,
											   const relation_engine& engine);
		[[nodiscard]] result<snapshot_store_v5_measurement>
		measure_snapshot_store_v5(snapshot_store_v5_graph_binding&& graph,
								  std::uint64_t maximum_bytes);
		[[nodiscard]] result<snapshot_store_v5_staging_binding>
		stream_snapshot_store_v5(snapshot_store_v5_measurement&& measurement,
								 std::unique_ptr<snapshot_store_v5_staging_sink> sink);
		[[nodiscard]] result<snapshot_store_v5_authenticated_cursor>
		take_snapshot_store_v5_cursor(snapshot_store_v5_staging_binding&& binding);
	} // namespace detail
} // namespace cxxlens::sdk
