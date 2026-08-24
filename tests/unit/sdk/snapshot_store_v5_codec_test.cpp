#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/sdk/store.hpp>

#include "sdk/snapshot_store_v5_codec_internal.hpp"
#include "sdk/store_identity_internal.hpp"

namespace
{
	using cxxlens::sdk::claim;
	using cxxlens::sdk::claim_input_basis_digest;
	using cxxlens::sdk::content_digest;
	using cxxlens::sdk::detached_cell;
	using cxxlens::sdk::detached_row;
	using cxxlens::sdk::error;
	using cxxlens::sdk::partition_draft;
	using cxxlens::sdk::publication_state;
	using cxxlens::sdk::relation_descriptor;
	using cxxlens::sdk::relation_engine;
	using cxxlens::sdk::relation_registry;
	using cxxlens::sdk::result;
	using cxxlens::sdk::snapshot_handle;
	using cxxlens::sdk::sqlite_bounded_byte_source;
	using cxxlens::sdk::sqlite_replayable_byte_source;
	using cxxlens::sdk::unexpected;
	using cxxlens::sdk::detail::snapshot_store_v5_authenticated_cursor;
	using cxxlens::sdk::detail::snapshot_store_v5_graph_binding;
	using cxxlens::sdk::detail::snapshot_store_v5_measurement;
	using cxxlens::sdk::detail::snapshot_store_v5_staging_binding;
	using cxxlens::sdk::detail::snapshot_store_v5_staging_sink;
} // namespace

namespace
{
	using namespace cxxlens::sdk;
	namespace detail = cxxlens::sdk::detail;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	template <class T>
	void require_error(const result<T>& value,
					   const std::string_view code,
					   const std::string_view field,
					   const std::string_view detail_value)
	{
		if (value && !code.empty())
			throw std::runtime_error{"expected an error tuple"};
		if (!value &&
			(value.error().code != code || value.error().field != field ||
			 value.error().detail != detail_value))
			throw std::runtime_error{"unexpected error tuple: " + value.error().code + "/" +
									 value.error().field + "/" + value.error().detail};
	}

	constexpr std::string_view producer_digest =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	constexpr std::string_view basis_digest =
		"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	constexpr std::string_view catalog_digest =
		"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
	constexpr std::string_view policy_digest =
		"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

	[[nodiscard]] relation_descriptor descriptor()
	{
		relation_descriptor value;
		value.id = "company.test.item.v1";
		value.name = "company.test.item";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.item/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.item.v1.key",
			 "key",
			 {scalar_kind::typed_id, "company_item_id", false},
			 true,
			 column_role::claim_key},
			{"company.test.item.v1.value",
			 "value",
			 {scalar_kind::interpretation_domain_id, {}, false},
			 true,
			 column_role::authoritative_payload},
		};
		value.key_columns = {"company.test.item.v1.key"};
		value.merge = merge_mode::set;
		value.descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] relation_engine engine()
	{
		relation_registry registry;
		require(registry.add(descriptor()).has_value(), "test relation descriptor was rejected");
		auto built = registry.build("engine-v5-codec-test");
		require(built.has_value(), "test relation engine was rejected");
		return std::move(*built);
	}

	[[nodiscard]] detached_row row(std::string key, std::string payload)
	{
		const auto relation = descriptor();
		row_builder builder{relation};
		require(builder
					.set({relation.id, relation.columns[0].id, relation.columns[0].type},
						 detached_cell::typed("company_item_id", std::move(key)))
					.has_value(),
				"test row key was rejected");
		require(builder
					.set({relation.id, relation.columns[1].id, relation.columns[1].type},
						 detached_cell{relation.columns[1].type,
									   cell_state::present,
									   scalar_value{std::move(payload)},
									   std::nullopt})
					.has_value(),
				"test row payload was rejected");
		auto finished = std::move(builder).finish();
		require(finished.has_value(), "test row did not finish");
		return std::move(*finished);
	}

	[[nodiscard]] claim
	make_claim(const relation_engine& value, std::string key, std::string payload)
	{
		observation observed{
			row(std::move(key), std::move(payload)),
			{"universe-1", {"all"}},
			"company.test.canonical-1",
			{"company.test.provider", std::string{producer_digest}},
			{std::string{basis_digest}},
			"evidence:root",
			{"exact", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto built = make_assertion(value, std::move(observed));
		require(built.has_value(), "test claim was rejected");
		return std::move(*built);
	}

	[[nodiscard]] partition_draft partition(const relation_engine& value, const bool reverse)
	{
		partition_draft draft;
		draft.relation_descriptor_id = descriptor().id;
		draft.scope = "compile-unit-1";
		draft.condition = {"universe-1", {"all"}};
		draft.interpretation = "company.test.canonical-1";
		draft.producer_semantics = std::string{producer_digest};
		draft.precision_profile = "exact";
		draft.assumption_set_id = "assumptions-empty";
		draft.claims = {make_claim(value, "item:1", "one"), make_claim(value, "item:2", "two")};
		auto basis = claim_input_basis_digest(draft.claims.front().input_basis);
		require(basis.has_value(), "test partition basis was rejected");
		draft.producer_input_basis_digest = std::move(*basis);
		draft.coverage = {{"compile-unit", "compile-unit-1", "covered", ""}};
		if (reverse)
			std::ranges::reverse(draft.claims);
		return draft;
	}

	[[nodiscard]] std::unique_ptr<snapshot_handle::data> build_graph(const relation_engine& value,
																	 const bool reverse = false)
	{
		auto graph = std::make_unique<snapshot_handle::data>();
		graph->query_annotations_available = true;
		graph->semantic_manifest.schema = "cxxlens.snapshot-manifest.v1";
		graph->semantic_manifest.snapshot_semantics_version = {1U, 0U, 0U};
		graph->semantic_manifest.catalog_semantic_digest = std::string{catalog_digest};
		graph->semantic_manifest.condition_universe_id = "universe-1";
		graph->semantic_manifest.relation_registry_digest = std::string{value.registry_digest()};
		graph->semantic_manifest.interpretation_policy_digest = std::string{policy_digest};
		auto draft = partition(value, reverse);
		auto manifest = make_partition_manifest(value, draft);
		require(manifest.has_value(), "test partition manifest was rejected");
		graph->semantic_manifest.partitions = {*manifest};
		graph->semantic_manifest.id = *detail::snapshot_manifest_identity(graph->semantic_manifest);
		graph->publication_record_value.series_id = "series:v5-codec";
		graph->publication_record_value.snapshot_id = graph->semantic_manifest.id;
		graph->publication_record_value.sequence = 1U;
		graph->publication_record_value.physical_generation = 7U;
		graph->publication_record_value.state = publication_state::committed;
		graph->publication_record_value.corrupt = false;
		graph->publication_record_value.publication_id = *detail::publication_record_identity(
			graph->publication_record_value.series_id,
			graph->publication_record_value.snapshot_id,
			graph->publication_record_value.sequence,
			graph->publication_record_value.parent_publication);
		graph->generation_pin = std::make_shared<const std::uint64_t>(
			graph->publication_record_value.physical_generation);
		graph->partition_envelopes.emplace(manifest->partition_id, draft);
		const auto relation = value.require_id(descriptor().id);
		require(relation.has_value(), "test relation was not found in the engine");
		graph->descriptors.emplace(descriptor().id, relation->descriptor());
		graph->rows.try_emplace(descriptor().id);
		graph->annotations.try_emplace(descriptor().id);
		graph->partition_bindings.push_back({manifest->partition_id,
											 draft.relation_descriptor_id,
											 draft.scope,
											 draft.condition,
											 draft.interpretation,
											 draft.producer_semantics,
											 draft.producer_input_basis_digest,
											 draft.precision_profile,
											 draft.assumption_set_id});
		for (const auto& coverage : draft.coverage)
			graph->coverage.push_back({draft.relation_descriptor_id, coverage});
		for (const auto& claim_value : draft.claims)
		{
			graph->rows[claim_value.descriptor].push_back(claim_value.row);
			graph->annotations[claim_value.descriptor].push_back({claim_value.row,
																  claim_value.presence,
																  claim_value.interpretation,
																  claim_value.semantic_key,
																  claim_value.assertion,
																  claim_value.content,
																  claim_value.producer,
																  claim_value.provenance_root,
																  claim_value.guarantee});
			graph->claim_contents.push_back(claim_value.content);
		}
		detail::sort_semantic_projections(*graph);
		return graph;
	}

	[[nodiscard]] std::unique_ptr<snapshot_handle::data> build_empty_graph()
	{
		auto graph = std::make_unique<snapshot_handle::data>();
		graph->query_annotations_available = true;
		graph->semantic_manifest.schema = "cxxlens.snapshot-manifest.v1";
		graph->semantic_manifest.snapshot_semantics_version = {1U, 0U, 0U};
		graph->semantic_manifest.catalog_semantic_digest = std::string{catalog_digest};
		graph->semantic_manifest.condition_universe_id = "universe-1";
		graph->semantic_manifest.relation_registry_digest =
			"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
		graph->semantic_manifest.interpretation_policy_digest = std::string{policy_digest};
		graph->semantic_manifest.id = *detail::snapshot_manifest_identity(graph->semantic_manifest);
		graph->publication_record_value.series_id = "series:v5-golden";
		graph->publication_record_value.snapshot_id = graph->semantic_manifest.id;
		graph->publication_record_value.sequence = 1U;
		graph->publication_record_value.physical_generation = 1U;
		graph->publication_record_value.state = publication_state::committed;
		graph->publication_record_value.publication_id = *detail::publication_record_identity(
			graph->publication_record_value.series_id,
			graph->publication_record_value.snapshot_id,
			graph->publication_record_value.sequence,
			graph->publication_record_value.parent_publication);
		graph->generation_pin = std::make_shared<const std::uint64_t>(
			graph->publication_record_value.physical_generation);
		return graph;
	}

	void rebind_snapshot_and_publication(snapshot_handle::data& graph)
	{
		graph.semantic_manifest.id = *detail::snapshot_manifest_identity(graph.semantic_manifest);
		graph.publication_record_value.snapshot_id = graph.semantic_manifest.id;
		graph.publication_record_value.publication_id =
			*detail::publication_record_identity(graph.publication_record_value.series_id,
												 graph.publication_record_value.snapshot_id,
												 graph.publication_record_value.sequence,
												 graph.publication_record_value.parent_publication);
	}

	void add_valid_closure(snapshot_handle::data& graph)
	{
		require(graph.semantic_manifest.partitions.size() == 1U &&
					graph.partition_bindings.size() == 1U,
				"closure fixture requires one partition");
		auto subject = make_partition_certificate_subject(
			graph.semantic_manifest.partitions.front(), graph.partition_bindings.front());
		require(subject.has_value(), "closure fixture subject was rejected");
		const auto& partition_value = graph.semantic_manifest.partitions.front();
		const auto& binding = graph.partition_bindings.front();
		closure_candidate candidate{
			partition_value.relation_descriptor_id,
			partition_value.partition_id,
			partition_value.content_digest,
			partition_value.coverage_digest,
			"sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
			binding.condition,
			binding.interpretation,
			binding.assumption_set_id,
			"relation-key-enumeration",
			binding.producer_semantics,
			"sha256:1111111111111111111111111111111111111111111111111111111111111111"};
		auto certificate = make_closure_certificate(*subject, std::move(candidate));
		require(certificate.has_value(), "closure fixture certificate was rejected");
		graph.semantic_manifest.closure_ids = {certificate->id};
		graph.closure_certificates = {std::move(*certificate)};
		rebind_snapshot_and_publication(graph);
	}

	[[nodiscard]] std::string bytes_hex(const std::span<const std::byte> bytes)
	{
		static constexpr std::string_view digits{"0123456789abcdef"};
		std::string output;
		output.reserve(bytes.size() * 2U);
		for (const auto byte : bytes)
		{
			const auto value = std::to_integer<unsigned char>(byte);
			output.push_back(digits[value >> 4U]);
			output.push_back(digits[value & 0x0fU]);
		}
		return output;
	}

	struct replay_state
	{
		std::vector<std::byte> bytes;
		std::size_t append_calls{};
		std::size_t seal_calls{};
		std::size_t discard_calls{};
		std::size_t open_calls{};
		std::size_t read_calls{};
		std::uint64_t returned_bytes{};
		std::size_t maximum_read_window{};
		std::size_t source_destructions{};
		std::size_t fragment_limit{7U};
		bool sealed{};
		bool discard_attempted{};
		bool fail_append{};
		bool fail_seal{};
		bool fail_discard{};
		bool fail_open{};
		bool fail_read{};
		bool null_open{};
		bool null_seal{};
		bool throw_append{};
		bool throw_seal{};
		bool throw_discard{};
		bool throw_open{};
		bool throw_read{};
		bool bad_alloc_append{};
		bool bad_alloc_seal{};
		bool bad_alloc_discard{};
		bool bad_alloc_open{};
		bool bad_alloc_read{};
		bool length_error_open{};
		bool length_error_read{};
		bool tamper_on_first_open{};
		bool over_return{};
	};

	class memory_cursor final : public sqlite_bounded_byte_source
	{
	  public:
		explicit memory_cursor(std::shared_ptr<replay_state> state) : state_{std::move(state)} {}

		[[nodiscard]] result<std::size_t> read(const std::span<std::byte> output) override
		{
			++state_->read_calls;
			state_->maximum_read_window = std::max(state_->maximum_read_window, output.size());
			if (state_->bad_alloc_read)
				throw std::bad_alloc{};
			if (state_->length_error_read)
				throw std::length_error{"test read length"};
			if (state_->throw_read)
				throw std::runtime_error{"test read exception"};
			if (state_->fail_read)
				return unexpected(error{"test.replay-source", "read", "fault"});
			if (state_->over_return)
				return output.size() + 1U;
			if (offset_ == state_->bytes.size())
				return std::size_t{};
			const auto count =
				std::min({output.size(), state_->fragment_limit, state_->bytes.size() - offset_});
			std::copy_n(state_->bytes.begin() + static_cast<std::ptrdiff_t>(offset_),
						count,
						output.begin());
			offset_ += count;
			state_->returned_bytes += static_cast<std::uint64_t>(count);
			return count;
		}

	  private:
		std::shared_ptr<replay_state> state_;
		std::size_t offset_{};
	};

	class memory_source final : public sqlite_replayable_byte_source
	{
	  public:
		explicit memory_source(std::shared_ptr<replay_state> state) : state_{std::move(state)} {}
		~memory_source() override
		{
			++state_->source_destructions;
		}

		[[nodiscard]] result<std::unique_ptr<sqlite_bounded_byte_source>> open_pass() const override
		{
			++state_->open_calls;
			if (state_->bad_alloc_open)
				throw std::bad_alloc{};
			if (state_->length_error_open)
				throw std::length_error{"test open length"};
			if (state_->throw_open)
				throw std::runtime_error{"test open exception"};
			if (state_->fail_open)
				return unexpected(error{"test.replay-source", "open-pass", "fault"});
			if (state_->null_open)
				return std::unique_ptr<sqlite_bounded_byte_source>{};
			if (state_->tamper_on_first_open && state_->open_calls == 1U && !state_->bytes.empty())
				state_->bytes.front() ^= std::byte{1U};
			std::unique_ptr<sqlite_bounded_byte_source> cursor =
				std::make_unique<memory_cursor>(state_);
			return cursor;
		}

	  private:
		std::shared_ptr<replay_state> state_;
	};

	class memory_sink final : public snapshot_store_v5_staging_sink
	{
	  public:
		explicit memory_sink(std::shared_ptr<replay_state> state) : state_{std::move(state)} {}
		~memory_sink() override = default;

		[[nodiscard]] result<void> append(const std::span<const std::byte> bytes) override
		{
			++state_->append_calls;
			if (state_->bad_alloc_append)
				throw std::bad_alloc{};
			if (state_->throw_append)
				throw std::runtime_error{"test append exception"};
			if (state_->fail_append)
				return unexpected(error{"test.replay-sink", "append", "fault"});
			if (state_->sealed)
				return unexpected(error{"test.replay-sink", "append", "sealed"});
			state_->bytes.insert(state_->bytes.end(), bytes.begin(), bytes.end());
			return {};
		}

		[[nodiscard]] result<std::shared_ptr<const sqlite_replayable_byte_source>> seal() override
		{
			++state_->seal_calls;
			if (state_->bad_alloc_seal)
				throw std::bad_alloc{};
			if (state_->throw_seal)
				throw std::runtime_error{"test seal exception"};
			if (state_->fail_seal)
				return unexpected(error{"test.replay-sink", "seal", "fault"});
			if (state_->sealed)
				return unexpected(error{"test.replay-sink", "seal", "sealed"});
			if (state_->null_seal)
				return std::shared_ptr<const sqlite_replayable_byte_source>{};
			state_->sealed = true;
			std::shared_ptr<const sqlite_replayable_byte_source> source =
				std::make_shared<memory_source>(state_);
			return source;
		}

		[[nodiscard]] result<void> discard_staging() override
		{
			++state_->discard_calls;
			state_->discard_attempted = true;
			if (state_->bad_alloc_discard)
				throw std::bad_alloc{};
			if (state_->throw_discard)
				throw std::runtime_error{"test discard exception"};
			if (state_->fail_discard)
				return unexpected(error{"store.cleanup-failed", "snapshot-v5-staging", "fault"});
			state_->bytes.clear();
			return {};
		}

	  private:
		std::shared_ptr<replay_state> state_;
	};

	[[nodiscard]] std::shared_ptr<replay_state> state_from(const std::vector<std::byte>& bytes = {})
	{
		auto state = std::make_shared<replay_state>();
		state->bytes = bytes;
		return state;
	}

	[[nodiscard]] result<snapshot_store_v5_graph_binding> seal_graph(const relation_engine& value,
																	 const bool reverse = false)
	{
		return detail::seal_snapshot_store_v5_reference_graph(build_graph(value, reverse), value);
	}

	[[nodiscard]] result<snapshot_store_v5_measurement> measure_graph(const relation_engine& value)
	{
		auto sealed = seal_graph(value);
		require(sealed.has_value(), "v5 fresh graph seal failed");
		return detail::measure_snapshot_store_v5(std::move(*sealed),
												 detail::snapshot_store_v5_maximum_payload_bytes);
	}

	[[nodiscard]] std::vector<std::byte> encode_graph(const snapshot_handle::data& value)
	{
		auto encoded = detail::encode_snapshot(value);
		require(encoded.has_value(), "v5 vector encode failed");
		return std::move(*encoded);
	}

	[[nodiscard]] std::vector<std::byte> read_all(snapshot_store_v5_authenticated_cursor& cursor)
	{
		std::vector<std::byte> output;
		std::array<std::byte, 5U> window{};
		for (;;)
		{
			auto read = cursor.read(window);
			require(read.has_value(), "fragmented authenticated cursor read failed");
			if (*read == 0U)
				break;
			output.insert(output.end(), window.begin(), window.begin() + *read);
		}
		return output;
	}

	void check_move_only()
	{
		static_assert(!std::is_copy_constructible_v<snapshot_store_v5_graph_binding>);
		static_assert(!std::is_copy_assignable_v<snapshot_store_v5_graph_binding>);
		static_assert(!std::is_copy_constructible_v<snapshot_store_v5_measurement>);
		static_assert(!std::is_copy_assignable_v<snapshot_store_v5_measurement>);
		static_assert(!std::is_copy_constructible_v<snapshot_store_v5_staging_binding>);
		static_assert(!std::is_copy_assignable_v<snapshot_store_v5_staging_binding>);
		static_assert(!std::is_copy_constructible_v<snapshot_store_v5_authenticated_cursor>);
		static_assert(!std::is_copy_assignable_v<snapshot_store_v5_authenticated_cursor>);
	}

	void check_golden_vector()
	{
		const auto bytes = encode_graph(*build_empty_graph());
		constexpr std::string_view expected_hex =
			"000000000000001e6378786c656e732e6e672d736e617073686f742d7061796c6f61642e76350000000000"
			"00001c6378"
			"786c656e732e736e617073686f742d6d616e69666573742e76310000000000000050736e617073686f743a"
			"7368613235"
			"363a3638336136613536393231373039663761326231336333303363303132666238326439333836333866"
			"6533616336"
			"61643761373933326638333738623134333600000000000000010000000000000000000000000000000000"
			"0000000000"
			"00477368613235363a63636363636363636363636363636363636363636363636363636363636363636363"
			"6363636363"
			"63636363636363636363636363636363636363636363636363000000000000000a756e6976657273652d31"
			"0000000000"
			"0000477368613235363a656565656565656565656565656565656565656565656565656565656565656565"
			"6565656565"
			"656565656565656565656565656565656565656565656565656500000000000000477368613235363a6464"
			"6464646464"
			"64646464646464646464646464646464646464646464646464646464646464646464646464646464646464"
			"6464646464"
			"6464646464646464640000000000000000000000000000000000000000000000537075626c69636174696f"
			"6e3a736861"
			"3235363a613262653063666134373539343535383735663033636462653435663036343337633432333963"
			"6366326438"
			"666531343062326231626636356566386230623200000000000000107365726965733a76352d676f6c6465"
			"6e00000000"
			"00000050736e617073686f743a7368613235363a3638336136613536393231373039663761326231336333"
			"3033633031"
			"32666238326439333836333866653361633661643761373933326638333738623134333600000000000000"
			"0100000000"
			"00000001000000000000000003000000000000000000000000000000000000000000000000000100000000"
			"0000000000"
			"00000000000000000000000000000000000000000000000000000000000000";
		constexpr std::string_view expected_digest =
			"sha256:b324033887b369e4d8aa26b1a2409d7ddee04a09ca19d5429891a11abada53c4";
		require(bytes_hex(bytes) == expected_hex, "v5 empty graph full-byte golden drifted");
		require(content_digest(bytes) == expected_digest, "v5 empty graph SHA golden drifted");
	}

	void check_determinism_and_vector_parity(const relation_engine& value)
	{
		const auto forward_bytes = encode_graph(*build_graph(value, false));
		const auto reverse_bytes = encode_graph(*build_graph(value, true));
		require(forward_bytes == reverse_bytes, "v5 permutation changed canonical bytes");
		require(detail::canonical_export_of(*build_graph(value, false)) ==
					detail::canonical_export_of(*build_graph(value, true)),
				"v5 permutation changed canonical export");
		require(detail::semantic_projection_bytes(*build_graph(value, false)) ==
					detail::semantic_projection_bytes(*build_graph(value, true)),
				"v5 permutation changed semantic projection");

		auto sealed = seal_graph(value);
		require(sealed.has_value(), "v5 graph seal failed");
		auto measured = detail::measure_snapshot_store_v5(
			std::move(*sealed), detail::snapshot_store_v5_maximum_payload_bytes);
		require(measured.has_value(), "v5 graph measure failed");
		auto state = state_from();
		auto staged = detail::stream_snapshot_store_v5(std::move(*measured),
													   std::make_unique<memory_sink>(state));
		require(staged.has_value(), "v5 stream failed");
		require(state->bytes == forward_bytes, "v5 vector and streamed bytes diverged");
		require(staged->observation().byte_count == forward_bytes.size() &&
					staged->observation().payload_sha256 == content_digest(forward_bytes),
				"v5 stream observation diverged from the canonical vector");
		require(state->discard_calls == 0U, "successful v5 stream discarded staging");
	}

	void check_bound_and_phase_tokens(const relation_engine& value)
	{
		auto discard_ledger_state = state_from();
		auto discard_ledger_sink = std::make_unique<memory_sink>(discard_ledger_state);
		auto first_discard = discard_ledger_sink->discard();
		require(first_discard.has_value(), "first staging discard was rejected");
		auto replayed_discard = discard_ledger_sink->discard();
		require_error(
			replayed_discard, "store.invariant-breach", "snapshot-v5-staging", "discard-replayed");
		require(discard_ledger_state->discard_calls == 1U,
				"staging sink implementation observed a replayed discard");

		auto first = seal_graph(value);
		require(first.has_value(), "v5 bound setup seal failed");
		auto measured = detail::measure_snapshot_store_v5(
			std::move(*first), detail::snapshot_store_v5_maximum_payload_bytes);
		require(measured.has_value() && measured->byte_count() > 1U,
				"v5 bound setup measure failed");
		const auto byte_count = measured->byte_count();

		auto too_small_binding = seal_graph(value);
		require(too_small_binding.has_value(), "v5 too-small setup seal failed");
		auto too_small =
			detail::measure_snapshot_store_v5(std::move(*too_small_binding), byte_count - 1U);
		require_error(too_small, "store.resource-limit", "snapshot-payload", "maximum-bytes");
		auto too_small_replay =
			detail::measure_snapshot_store_v5(std::move(*too_small_binding), byte_count);
		require_error(
			too_small_replay, "store.invariant-breach", "snapshot-v5-measure", "moved-or-replayed");

		auto zero_binding = seal_graph(value);
		require(zero_binding.has_value(), "v5 zero-bound setup seal failed");
		auto zero = detail::measure_snapshot_store_v5(std::move(*zero_binding), 0U);
		require_error(zero, "store.resource-limit", "snapshot-payload", "maximum-bytes");

		auto too_large_binding = seal_graph(value);
		require(too_large_binding.has_value(), "v5 too-large setup seal failed");
		auto too_large = detail::measure_snapshot_store_v5(
			std::move(*too_large_binding), detail::snapshot_store_v5_maximum_payload_bytes + 1U);
		require_error(too_large, "store.resource-limit", "snapshot-payload", "maximum-bytes");

		auto replayed_binding = seal_graph(value);
		require(replayed_binding.has_value(), "v5 replay setup seal failed");
		auto replayed_measurement = detail::measure_snapshot_store_v5(
			std::move(*replayed_binding), detail::snapshot_store_v5_maximum_payload_bytes);
		require(replayed_measurement.has_value(), "v5 replay setup measure failed");
		auto replayed_binding_again = detail::measure_snapshot_store_v5(
			std::move(*replayed_binding), detail::snapshot_store_v5_maximum_payload_bytes);
		require_error(replayed_binding_again,
					  "store.invariant-breach",
					  "snapshot-v5-measure",
					  "moved-or-replayed");
		auto replay_sink_state = state_from();
		auto replayed_stream = detail::stream_snapshot_store_v5(
			std::move(*replayed_measurement), std::make_unique<memory_sink>(replay_sink_state));
		require(replayed_stream.has_value(), "v5 replay setup stream failed");
		auto unused_replay_sink = state_from();
		auto replayed_stream_again = detail::stream_snapshot_store_v5(
			std::move(*replayed_measurement), std::make_unique<memory_sink>(unused_replay_sink));
		require_error(replayed_stream_again,
					  "store.invariant-breach",
					  "snapshot-v5-stream",
					  "moved-or-replayed");
		require(unused_replay_sink->append_calls == 0U && unused_replay_sink->seal_calls == 0U &&
					unused_replay_sink->discard_calls == 0U,
				"replayed measurement touched a staging sink");
		auto replayed_staging_again =
			detail::take_snapshot_store_v5_cursor(std::move(*replayed_stream));
		require(replayed_staging_again.has_value(), "v5 replay cursor setup failed");
		auto replayed_cursor_again =
			detail::take_snapshot_store_v5_cursor(std::move(*replayed_stream));
		require_error(replayed_cursor_again,
					  "store.invariant-breach",
					  "snapshot-v5-cursor",
					  "moved-or-replayed");

		auto null_sink_measurement = measure_graph(value);
		require(null_sink_measurement.has_value(), "v5 null-sink setup failed");
		auto null_sink = detail::stream_snapshot_store_v5(
			std::move(*null_sink_measurement), std::unique_ptr<snapshot_store_v5_staging_sink>{});
		require_error(null_sink, "store.invariant-breach", "snapshot-v5-stream", "null-sink");
		auto null_sink_replay_state = state_from();
		auto null_sink_replay =
			detail::stream_snapshot_store_v5(std::move(*null_sink_measurement),
											 std::make_unique<memory_sink>(null_sink_replay_state));
		require_error(
			null_sink_replay, "store.invariant-breach", "snapshot-v5-stream", "moved-or-replayed");
		require(null_sink_replay_state->append_calls == 0U &&
					null_sink_replay_state->seal_calls == 0U &&
					null_sink_replay_state->discard_calls == 0U,
				"replayed null-sink measurement touched staging");
	}

	void check_faults_and_authenticated_cursor(const relation_engine& value)
	{
		auto measured_graph = seal_graph(value);
		require(measured_graph.has_value(), "v5 fault setup seal failed");
		auto measured = detail::measure_snapshot_store_v5(
			std::move(*measured_graph), detail::snapshot_store_v5_maximum_payload_bytes);
		require(measured.has_value(), "v5 fault setup measure failed");
		const auto expected_bytes = encode_graph(*build_graph(value));

		auto state = state_from();
		auto staged = detail::stream_snapshot_store_v5(std::move(*measured),
													   std::make_unique<memory_sink>(state));
		require(staged.has_value(), "v5 authenticated stream failed");
		require(state->append_calls != 0U && state->seal_calls == 1U && state->open_calls == 1U &&
					state->discard_calls == 0U,
				"v5 stream did not perform the expected sink/source phases");
		auto cursor = detail::take_snapshot_store_v5_cursor(std::move(*staged));
		require(cursor.has_value(), "v5 authenticated cursor setup failed");
		require(state->source_destructions == 0U,
				"v5 cursor released its sealed replay backing before the pass completed");
		const auto reread = read_all(*cursor);
		require(reread == expected_bytes, "v5 authenticated fragmented reread changed bytes");
		auto observation = cursor->finish();
		require(observation.has_value() && observation->byte_count == expected_bytes.size() &&
					observation->payload_sha256 == content_digest(expected_bytes),
				"v5 authenticated cursor finish receipt diverged");
		auto replayed_finish = cursor->finish();
		require_error(
			replayed_finish, "store.invariant-breach", "snapshot-v5-cursor", "moved-or-finished");
		require(state->open_calls == 2U && state->source_destructions == 1U,
				"v5 cursor did not retain and release sealed source custody exactly once");

		auto dropped_state = state_from();
		{
			auto dropped_measurement = measure_graph(value);
			require(dropped_measurement.has_value(), "v5 dropped-custody setup failed");
			auto dropped_staging = detail::stream_snapshot_store_v5(
				std::move(*dropped_measurement), std::make_unique<memory_sink>(dropped_state));
			require(dropped_staging.has_value(), "v5 dropped-custody stream failed");
			require(dropped_state->source_destructions == 0U,
					"v5 sealed source was released before staging custody ended");
		}
		require(dropped_state->source_destructions == 1U,
				"dropping v5 staging custody did not release its sealed source exactly once");

		auto append_state = state_from();
		append_state->fail_append = true;
		auto append_measurement = measure_graph(value);
		require(append_measurement.has_value(), "v5 append fault setup failed");
		auto append_fault = detail::stream_snapshot_store_v5(
			std::move(*append_measurement), std::make_unique<memory_sink>(append_state));
		require_error(append_fault, "test.replay-sink", "append", "fault");
		require(append_state->discard_calls == 1U && append_state->bytes.empty(),
				"append fault did not discard partial staging exactly once");

		auto seal_state = state_from();
		seal_state->fail_seal = true;
		auto seal_measurement = measure_graph(value);
		require(seal_measurement.has_value(), "v5 seal fault setup failed");
		auto seal_fault = detail::stream_snapshot_store_v5(
			std::move(*seal_measurement), std::make_unique<memory_sink>(seal_state));
		require_error(seal_fault, "test.replay-sink", "seal", "fault");
		require(seal_state->discard_calls == 1U && seal_state->bytes.empty(),
				"seal fault did not discard partial staging exactly once");

		auto seal_exception_state = state_from();
		seal_exception_state->throw_seal = true;
		auto seal_exception_measurement = measure_graph(value);
		require(seal_exception_measurement.has_value(), "v5 seal exception setup failed");
		auto seal_exception =
			detail::stream_snapshot_store_v5(std::move(*seal_exception_measurement),
											 std::make_unique<memory_sink>(seal_exception_state));
		require_error(seal_exception, "store.invariant-breach", "snapshot-v5-stream", "exception");
		require(seal_exception_state->discard_calls == 1U,
				"seal exception did not discard staging exactly once");

		auto append_exception_state = state_from();
		append_exception_state->throw_append = true;
		auto append_exception_measurement = measure_graph(value);
		require(append_exception_measurement.has_value(), "v5 append exception setup failed");
		auto append_exception =
			detail::stream_snapshot_store_v5(std::move(*append_exception_measurement),
											 std::make_unique<memory_sink>(append_exception_state));
		require_error(
			append_exception, "store.invariant-breach", "snapshot-v5-stream", "exception");
		require(append_exception_state->discard_calls == 1U,
				"append exception did not discard staging exactly once");

		auto discard_failure_state = state_from();
		discard_failure_state->fail_append = true;
		discard_failure_state->fail_discard = true;
		auto discard_failure_measurement = measure_graph(value);
		require(discard_failure_measurement.has_value(), "v5 discard failure setup failed");
		auto discard_failure =
			detail::stream_snapshot_store_v5(std::move(*discard_failure_measurement),
											 std::make_unique<memory_sink>(discard_failure_state));
		require_error(discard_failure, "store.cleanup-failed", "snapshot-v5-staging", "fault");
		require(discard_failure_state->discard_calls == 1U, "discard failure retried cleanup");

		auto null_seal_state = state_from();
		null_seal_state->null_seal = true;
		auto null_seal_measurement = measure_graph(value);
		require(null_seal_measurement.has_value(), "v5 null-seal setup failed");
		auto null_seal = detail::stream_snapshot_store_v5(
			std::move(*null_seal_measurement), std::make_unique<memory_sink>(null_seal_state));
		require_error(
			null_seal, "store.invariant-breach", "snapshot-v5-stream", "null-sealed-source");
		require(null_seal_state->discard_calls == 1U && null_seal_state->bytes.empty(),
				"null sealed source did not discard staging exactly once");

		auto append_allocation_state = state_from();
		append_allocation_state->bad_alloc_append = true;
		auto append_allocation_measurement = measure_graph(value);
		require(append_allocation_measurement.has_value(), "v5 append allocation setup failed");
		auto append_allocation = detail::stream_snapshot_store_v5(
			std::move(*append_allocation_measurement),
			std::make_unique<memory_sink>(append_allocation_state));
		require_error(
			append_allocation, "store.allocation-failure", "snapshot-v5-stream", "stream");
		require(append_allocation_state->discard_calls == 1U,
				"append allocation failure did not discard staging exactly once");

		auto seal_allocation_state = state_from();
		seal_allocation_state->bad_alloc_seal = true;
		auto seal_allocation_measurement = measure_graph(value);
		require(seal_allocation_measurement.has_value(), "v5 seal allocation setup failed");
		auto seal_allocation =
			detail::stream_snapshot_store_v5(std::move(*seal_allocation_measurement),
											 std::make_unique<memory_sink>(seal_allocation_state));
		require_error(seal_allocation, "store.allocation-failure", "snapshot-v5-stream", "stream");
		require(seal_allocation_state->discard_calls == 1U,
				"seal allocation failure did not discard staging exactly once");

		auto discard_allocation_state = state_from();
		discard_allocation_state->fail_append = true;
		discard_allocation_state->bad_alloc_discard = true;
		auto discard_allocation_measurement = measure_graph(value);
		require(discard_allocation_measurement.has_value(), "v5 discard allocation setup failed");
		auto discard_allocation = detail::stream_snapshot_store_v5(
			std::move(*discard_allocation_measurement),
			std::make_unique<memory_sink>(discard_allocation_state));
		require_error(discard_allocation,
					  "store.cleanup-failed",
					  "snapshot-v5-staging",
					  "discard-allocation");
		require(discard_allocation_state->discard_calls == 1U,
				"discard allocation failure retried cleanup");

		auto discard_exception_state = state_from();
		discard_exception_state->fail_append = true;
		discard_exception_state->throw_discard = true;
		auto discard_exception_measurement = measure_graph(value);
		require(discard_exception_measurement.has_value(), "v5 discard exception setup failed");
		auto discard_exception = detail::stream_snapshot_store_v5(
			std::move(*discard_exception_measurement),
			std::make_unique<memory_sink>(discard_exception_state));
		require_error(
			discard_exception, "store.cleanup-failed", "snapshot-v5-staging", "discard-exception");
		require(discard_exception_state->discard_calls == 1U, "discard exception retried cleanup");

		auto comparison_state = state_from();
		comparison_state->tamper_on_first_open = true;
		auto comparison_measurement = measure_graph(value);
		require(comparison_measurement.has_value(), "v5 comparison tamper setup failed");
		auto comparison_fault = detail::stream_snapshot_store_v5(
			std::move(*comparison_measurement), std::make_unique<memory_sink>(comparison_state));
		require_error(comparison_fault, "store.corrupt", "payload", "noncanonical");
		require(comparison_state->discard_calls == 0U &&
					comparison_state->source_destructions == 1U,
				"sealed comparison failure did not release source custody");

		auto read_state = state_from();
		auto read_measurement = measure_graph(value);
		require(read_measurement.has_value(), "v5 read fault setup failed");
		auto read_staged = detail::stream_snapshot_store_v5(
			std::move(*read_measurement), std::make_unique<memory_sink>(read_state));
		require(read_staged.has_value(), "v5 read fault stream failed");
		read_state->fail_read = true;
		auto read_cursor = detail::take_snapshot_store_v5_cursor(std::move(*read_staged));
		require(read_cursor.has_value(), "v5 read fault cursor setup failed");
		std::array<std::byte, 5U> read_window{};
		auto read_fault = read_cursor->read(read_window);
		require_error(read_fault, "test.replay-source", "read", "fault");

		auto tamper_state = state_from();
		auto tamper_measurement = measure_graph(value);
		require(tamper_measurement.has_value(), "v5 tamper setup failed");
		auto tamper_staged = detail::stream_snapshot_store_v5(
			std::move(*tamper_measurement), std::make_unique<memory_sink>(tamper_state));
		require(tamper_staged.has_value() && !tamper_state->bytes.empty(),
				"v5 tamper stream failed");
		tamper_state->bytes.front() ^= std::byte{1U};
		auto tamper_cursor = detail::take_snapshot_store_v5_cursor(std::move(*tamper_staged));
		require(tamper_cursor.has_value(), "v5 tamper cursor setup failed");
		(void)read_all(*tamper_cursor);
		auto tamper_finish = tamper_cursor->finish();
		require_error(tamper_finish, "store.corrupt", "snapshot-v5-staging", "cursor-digest");

		auto over_return_state = state_from();
		auto over_return_measurement = measure_graph(value);
		require(over_return_measurement.has_value(), "v5 cursor-window setup failed");
		auto over_return_staged = detail::stream_snapshot_store_v5(
			std::move(*over_return_measurement), std::make_unique<memory_sink>(over_return_state));
		require(over_return_staged.has_value(), "v5 cursor-window stream failed");
		over_return_state->over_return = true;
		auto over_return_cursor =
			detail::take_snapshot_store_v5_cursor(std::move(*over_return_staged));
		require(over_return_cursor.has_value(), "v5 cursor-window cursor setup failed");
		auto over_return_fault = over_return_cursor->read(read_window);
		require_error(over_return_fault, "store.corrupt", "snapshot-v5-staging", "cursor-window");

		auto bounded_window_state = state_from();
		auto bounded_window_measurement = measure_graph(value);
		require(bounded_window_measurement.has_value(), "v5 bounded-window setup failed");
		auto bounded_window_staged =
			detail::stream_snapshot_store_v5(std::move(*bounded_window_measurement),
											 std::make_unique<memory_sink>(bounded_window_state));
		require(bounded_window_staged.has_value(), "v5 bounded-window stream failed");
		const auto bounded_byte_count = bounded_window_staged->observation().byte_count;
		auto bounded_window_cursor =
			detail::take_snapshot_store_v5_cursor(std::move(*bounded_window_staged));
		require(bounded_window_cursor.has_value(), "v5 bounded-window cursor setup failed");
		bounded_window_state->maximum_read_window = 0U;
		bounded_window_state->fragment_limit = std::numeric_limits<std::size_t>::max();
		std::vector<std::byte> oversized_window(static_cast<std::size_t>(bounded_byte_count) + 64U);
		auto bounded_read = bounded_window_cursor->read(oversized_window);
		require(bounded_read.has_value() && *bounded_read == bounded_byte_count,
				"v5 bounded cursor did not return its exact remaining payload");
		require(bounded_window_state->maximum_read_window == bounded_byte_count,
				"v5 bounded cursor exposed an oversized backend read window");

		auto open_state = state_from();
		auto open_measurement = measure_graph(value);
		require(open_measurement.has_value(), "v5 open fault setup failed");
		auto open_staged = detail::stream_snapshot_store_v5(
			std::move(*open_measurement), std::make_unique<memory_sink>(open_state));
		require(open_staged.has_value(), "v5 open fault stream failed");
		open_state->fail_open = true;
		auto open_fault = detail::take_snapshot_store_v5_cursor(std::move(*open_staged));
		require_error(open_fault, "test.replay-source", "open-pass", "fault");

		auto open_allocation_state = state_from();
		auto open_allocation_measurement = measure_graph(value);
		require(open_allocation_measurement.has_value(), "v5 open allocation setup failed");
		auto open_allocation_staged =
			detail::stream_snapshot_store_v5(std::move(*open_allocation_measurement),
											 std::make_unique<memory_sink>(open_allocation_state));
		require(open_allocation_staged.has_value(), "v5 open allocation stream failed");
		open_allocation_state->bad_alloc_open = true;
		auto open_allocation =
			detail::take_snapshot_store_v5_cursor(std::move(*open_allocation_staged));
		require_error(open_allocation, "store.allocation-failure", "snapshot-v5-cursor", "open");

		auto early_state = state_from();
		auto early_measurement = measure_graph(value);
		require(early_measurement.has_value(), "v5 early-finish setup failed");
		auto early_staged = detail::stream_snapshot_store_v5(
			std::move(*early_measurement), std::make_unique<memory_sink>(early_state));
		require(early_staged.has_value(), "v5 early-finish stream failed");
		auto early_cursor = detail::take_snapshot_store_v5_cursor(std::move(*early_staged));
		require(early_cursor.has_value(), "v5 early-finish cursor setup failed");
		auto early_finish = early_cursor->finish();
		require_error(early_finish, "store.corrupt", "snapshot-v5-staging", "cursor-size");

		auto empty_state = state_from();
		auto empty_measurement = measure_graph(value);
		require(empty_measurement.has_value(), "v5 empty-window setup failed");
		auto empty_staged = detail::stream_snapshot_store_v5(
			std::move(*empty_measurement), std::make_unique<memory_sink>(empty_state));
		require(empty_staged.has_value(), "v5 empty-window stream failed");
		auto empty_cursor = detail::take_snapshot_store_v5_cursor(std::move(*empty_staged));
		require(empty_cursor.has_value(), "v5 empty-window cursor setup failed");
		auto empty_read = empty_cursor->read({});
		require_error(empty_read, "store.invariant-breach", "snapshot-v5-cursor", "empty-window");

		auto premature_state = state_from();
		auto premature_measurement = measure_graph(value);
		require(premature_measurement.has_value(), "v5 premature-EOF setup failed");
		auto premature_staged = detail::stream_snapshot_store_v5(
			std::move(*premature_measurement), std::make_unique<memory_sink>(premature_state));
		require(premature_staged.has_value() && !premature_state->bytes.empty(),
				"v5 premature-EOF stream failed");
		premature_state->bytes.pop_back();
		auto premature_cursor = detail::take_snapshot_store_v5_cursor(std::move(*premature_staged));
		require(premature_cursor.has_value(), "v5 premature-EOF cursor setup failed");
		(void)read_all(*premature_cursor);
		auto premature_finish = premature_cursor->finish();
		require_error(premature_finish, "store.corrupt", "snapshot-v5-staging", "cursor-size");

		auto extra_state = state_from();
		auto extra_measurement = measure_graph(value);
		require(extra_measurement.has_value(), "v5 extra-byte setup failed");
		auto extra_staged = detail::stream_snapshot_store_v5(
			std::move(*extra_measurement), std::make_unique<memory_sink>(extra_state));
		require(extra_staged.has_value(), "v5 extra-byte stream failed");
		const auto expected_count = extra_staged->observation().byte_count;
		extra_state->bytes.push_back(std::byte{0x7fU});
		auto extra_cursor = detail::take_snapshot_store_v5_cursor(std::move(*extra_staged));
		require(extra_cursor.has_value(), "v5 extra-byte cursor setup failed");
		std::uint64_t consumed{};
		while (consumed < expected_count)
		{
			const auto remaining = expected_count - consumed;
			auto read = extra_cursor->read(std::span{read_window}.first(
				static_cast<std::size_t>(std::min<std::uint64_t>(remaining, read_window.size()))));
			require(read.has_value() && *read != 0U, "v5 extra-byte cursor ended early");
			consumed += static_cast<std::uint64_t>(*read);
		}
		auto extra_finish = extra_cursor->finish();
		require_error(extra_finish, "store.corrupt", "snapshot-v5-staging", "cursor-size");

		auto null_open_state = state_from();
		auto null_open_measurement = measure_graph(value);
		require(null_open_measurement.has_value(), "v5 null-open setup failed");
		auto null_open_staged = detail::stream_snapshot_store_v5(
			std::move(*null_open_measurement), std::make_unique<memory_sink>(null_open_state));
		require(null_open_staged.has_value(), "v5 null-open stream failed");
		null_open_state->null_open = true;
		auto null_open = detail::take_snapshot_store_v5_cursor(std::move(*null_open_staged));
		require_error(
			null_open, "store.invariant-breach", "snapshot-v5-cursor", "null-replay-pass");

		auto open_exception_state = state_from();
		auto open_exception_measurement = measure_graph(value);
		require(open_exception_measurement.has_value(), "v5 open exception setup failed");
		auto open_exception_staged =
			detail::stream_snapshot_store_v5(std::move(*open_exception_measurement),
											 std::make_unique<memory_sink>(open_exception_state));
		require(open_exception_staged.has_value(), "v5 open exception stream failed");
		open_exception_state->throw_open = true;
		auto open_exception =
			detail::take_snapshot_store_v5_cursor(std::move(*open_exception_staged));
		require_error(open_exception, "store.invariant-breach", "snapshot-v5-cursor", "exception");

		auto read_exception_state = state_from();
		auto read_exception_measurement = measure_graph(value);
		require(read_exception_measurement.has_value(), "v5 read exception setup failed");
		auto read_exception_staged =
			detail::stream_snapshot_store_v5(std::move(*read_exception_measurement),
											 std::make_unique<memory_sink>(read_exception_state));
		require(read_exception_staged.has_value(), "v5 read exception stream failed");
		auto read_exception_cursor =
			detail::take_snapshot_store_v5_cursor(std::move(*read_exception_staged));
		require(read_exception_cursor.has_value(), "v5 read exception cursor setup failed");
		read_exception_state->throw_read = true;
		auto read_exception = read_exception_cursor->read(read_window);
		require_error(
			read_exception, "store.invariant-breach", "snapshot-v5-cursor", "read-exception");

		auto read_allocation_state = state_from();
		auto read_allocation_measurement = measure_graph(value);
		require(read_allocation_measurement.has_value(), "v5 read allocation setup failed");
		auto read_allocation_staged =
			detail::stream_snapshot_store_v5(std::move(*read_allocation_measurement),
											 std::make_unique<memory_sink>(read_allocation_state));
		require(read_allocation_staged.has_value(), "v5 read allocation stream failed");
		auto read_allocation_cursor =
			detail::take_snapshot_store_v5_cursor(std::move(*read_allocation_staged));
		require(read_allocation_cursor.has_value(), "v5 read allocation cursor setup failed");
		read_allocation_state->bad_alloc_read = true;
		auto read_allocation = read_allocation_cursor->read(read_window);
		require_error(read_allocation, "store.allocation-failure", "snapshot-v5-cursor", "read");
	}

	void check_decoder_bounds_and_roundtrip(const relation_engine& value)
	{
		auto reference = build_graph(value);
		const auto expected_export = detail::canonical_export_of(*reference);
		const auto expected_projection = detail::semantic_projection_bytes(*reference);
		const auto bytes = encode_graph(*reference);
		auto roundtrip_state = state_from(bytes);
		memory_source roundtrip_source{roundtrip_state};
		auto decoded = detail::decode_snapshot(roundtrip_source, bytes.size(), value);
		require(decoded.has_value(), "valid canonical v5 payload did not decode");
		require(encode_graph(**decoded) == bytes,
				"decoded v5 payload did not roundtrip exact bytes");
		require(detail::canonical_export_of(**decoded) == expected_export,
				"decoded v5 payload changed canonical export");
		require(detail::semantic_projection_bytes(**decoded) == expected_projection,
				"decoded v5 payload changed semantic projection");
		require(roundtrip_state->open_calls == 2U &&
					roundtrip_state->returned_bytes == 2U * bytes.size(),
				"canonical v5 decoder did not complete two exact bounded passes");

		auto empty_payload_state = state_from();
		memory_source empty_payload_source{empty_payload_state};
		auto empty_payload = detail::decode_snapshot(empty_payload_source, 0U, value);
		require_error(empty_payload, "store.corrupt", "payload", "format");
		require(empty_payload_state->open_calls == 1U,
				"empty v5 payload bypassed the normal decoder classification");

		auto oversized_payload_state = state_from(bytes);
		memory_source oversized_payload_source{oversized_payload_state};
		auto oversized_payload = detail::decode_snapshot(
			oversized_payload_source, detail::snapshot_store_v5_maximum_payload_bytes + 1U, value);
		require_error(
			oversized_payload, "store.resource-limit", "snapshot-payload", "maximum-bytes");
		require(oversized_payload_state->open_calls == 0U,
				"oversized v5 payload opened its source before bound rejection");

		auto huge_schema_length = bytes;
		constexpr std::size_t schema_length_offset = 8U + 30U;
		require(huge_schema_length.size() >= schema_length_offset + 8U,
				"v5 decoder fixture header was truncated");
		std::fill_n(huge_schema_length.begin() + static_cast<std::ptrdiff_t>(schema_length_offset),
					8U,
					std::byte{0xffU});
		auto huge_schema_state = state_from(huge_schema_length);
		memory_source huge_schema_source{huge_schema_state};
		auto huge_schema =
			detail::decode_snapshot(huge_schema_source, huge_schema_length.size(), value);
		require_error(huge_schema, "store.corrupt", "manifest", "header");
		require(huge_schema_state->open_calls == 1U &&
					huge_schema_state->returned_bytes == schema_length_offset + 8U,
				"huge v5 schema length read or allocated beyond its bounded header");

		auto generation_state = state_from(bytes);
		memory_source generation_source{generation_state};
		auto generation_offset = detail::payload_generation_offset(
			generation_source,
			bytes.size(),
			reference->publication_record_value.physical_generation);
		require(generation_offset.has_value() && *generation_offset < bytes.size(),
				"valid v5 generation offset was rejected");
		auto oversized_generation_state = state_from(bytes);
		memory_source oversized_generation_source{oversized_generation_state};
		auto oversized_generation_offset = detail::payload_generation_offset(
			oversized_generation_source,
			detail::snapshot_store_v5_maximum_payload_bytes + 1U,
			reference->publication_record_value.physical_generation);
		require_error(oversized_generation_offset,
					  "store.resource-limit",
					  "snapshot-payload",
					  "maximum-bytes");
		require(oversized_generation_state->open_calls == 0U,
				"oversized generation-offset source opened before bound rejection");

		auto oversized_row_count_bytes = bytes;
		constexpr std::uint64_t rejected_row_count = 10'000'001U;
		const auto relation_count_offset = *generation_offset + 8U + 1U + 8U + 1U;
		const auto row_count_offset = relation_count_offset + 8U + 8U + descriptor().id.size();
		require(oversized_row_count_bytes.size() >= row_count_offset + 8U,
				"v5 row-count fixture header was truncated");
		for (std::size_t index = 0U; index < 8U; ++index)
		{
			const auto shift = static_cast<unsigned>((7U - index) * 8U);
			oversized_row_count_bytes[row_count_offset + index] =
				static_cast<std::byte>((rejected_row_count >> shift) & 0xffU);
		}
		auto oversized_row_count_state = state_from(oversized_row_count_bytes);
		memory_source oversized_row_count_source{oversized_row_count_state};
		auto oversized_row_count = detail::decode_snapshot(
			oversized_row_count_source, oversized_row_count_bytes.size(), value);
		require_error(oversized_row_count, "store.corrupt", "rows", "header");
		require(oversized_row_count_state->open_calls == 1U &&
					oversized_row_count_state->returned_bytes <= row_count_offset + 8U,
				"oversized v5 row count allocated or read beyond its bounded header");

		auto allocation_open_state = state_from(bytes);
		allocation_open_state->bad_alloc_open = true;
		memory_source allocation_open_source{allocation_open_state};
		auto allocation_open = detail::decode_snapshot(allocation_open_source, bytes.size(), value);
		require_error(
			allocation_open, "store.allocation-failure", "snapshot-v5-decode", "allocation");

		auto allocation_read_state = state_from(bytes);
		allocation_read_state->bad_alloc_read = true;
		memory_source allocation_read_source{allocation_read_state};
		auto allocation_read = detail::decode_snapshot(allocation_read_source, bytes.size(), value);
		require_error(
			allocation_read, "store.allocation-failure", "snapshot-v5-decode", "allocation");

		auto length_read_state = state_from(bytes);
		length_read_state->length_error_read = true;
		memory_source length_read_source{length_read_state};
		auto length_read = detail::decode_snapshot(length_read_source, bytes.size(), value);
		require_error(length_read, "store.resource-limit", "snapshot-payload", "maximum-bytes");
	}

	void check_invalid_bindings(const relation_engine& value)
	{
		auto invalid_snapshot = build_graph(value);
		invalid_snapshot->semantic_manifest.id = "snapshot:sha256:" + std::string(64U, '0');
		auto rejected_snapshot =
			detail::seal_snapshot_store_v5_reference_graph(std::move(invalid_snapshot), value);
		require_error(
			rejected_snapshot, "store.corrupt", "snapshot-v5-binding", "snapshot-identity");

		auto invalid_publication = build_graph(value);
		invalid_publication->publication_record_value.publication_id =
			"publication:sha256:" + std::string(64U, '0');
		auto rejected_publication =
			detail::seal_snapshot_store_v5_reference_graph(std::move(invalid_publication), value);
		require_error(
			rejected_publication, "store.corrupt", "snapshot-v5-binding", "publication-identity");

		auto missing_generation_pin = build_graph(value);
		missing_generation_pin->generation_pin.reset();
		auto rejected_missing_generation_pin = detail::seal_snapshot_store_v5_reference_graph(
			std::move(missing_generation_pin), value);
		require_error(rejected_missing_generation_pin,
					  "store.corrupt",
					  "snapshot-v5-binding",
					  "generation-custody");

		auto foreign_generation_pin = build_graph(value);
		foreign_generation_pin->generation_pin = std::make_shared<const std::uint64_t>(8U);
		auto rejected_foreign_generation_pin = detail::seal_snapshot_store_v5_reference_graph(
			std::move(foreign_generation_pin), value);
		require_error(rejected_foreign_generation_pin,
					  "store.corrupt",
					  "snapshot-v5-binding",
					  "generation-custody");

		auto missing_query_annotations = build_graph(value);
		missing_query_annotations->query_annotations_available = false;
		auto rejected_missing_query_annotations = detail::seal_snapshot_store_v5_reference_graph(
			std::move(missing_query_annotations), value);
		require_error(rejected_missing_query_annotations,
					  "store.corrupt",
					  "snapshot-v5-binding",
					  "query-annotations");

		auto duplicate_partition = build_graph(value);
		duplicate_partition->semantic_manifest.partitions.push_back(
			duplicate_partition->semantic_manifest.partitions.front());
		rebind_snapshot_and_publication(*duplicate_partition);
		auto rejected_duplicate_partition =
			detail::seal_snapshot_store_v5_reference_graph(std::move(duplicate_partition), value);
		require_error(rejected_duplicate_partition,
					  "store.corrupt",
					  "snapshot-v5-binding",
					  "noncanonical-order");

		auto valid_closure = build_graph(value);
		add_valid_closure(*valid_closure);
		auto accepted_closure =
			detail::seal_snapshot_store_v5_reference_graph(std::move(valid_closure), value);
		require(accepted_closure.has_value(), "valid v5 closure binding was rejected");

		auto forged_closure = build_graph(value);
		add_valid_closure(*forged_closure);
		forged_closure->closure_certificates.front().subject.evidence_digest =
			"sha256:2222222222222222222222222222222222222222222222222222222222222222";
		auto rejected_forged_closure =
			detail::seal_snapshot_store_v5_reference_graph(std::move(forged_closure), value);
		require_error(
			rejected_forged_closure, "store.corrupt", "snapshot-v5-binding", "closure-identity");
	}
} // namespace

int main()
{
	try
	{
		check_move_only();
		check_golden_vector();
		const auto value = engine();
		check_determinism_and_vector_parity(value);
		check_bound_and_phase_tokens(value);
		check_faults_and_authenticated_cursor(value);
		check_decoder_bounds_and_roundtrip(value);
		check_invalid_bindings(value);
		return 0;
	}
	catch (const std::exception& failure)
	{
		std::cerr << failure.what() << '\n';
		return 1;
	}
}
