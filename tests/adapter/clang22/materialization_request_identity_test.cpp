#include "llvm/clang22/materialization_request_identity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang22/materialization_admission_error.hpp"
#include "llvm/clang22/materialization_identity.hpp"

namespace
{
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::span<const std::byte> bytes(const std::string_view value)
	{
		return std::as_bytes(std::span{value.data(), value.size()});
	}

	class fragmented_spool final : public materialization_replayable_spool
	{
	  public:
		fragmented_spool(std::unique_ptr<materialization_replayable_spool> delegate,
						 const std::size_t maximum_fragment)
			: delegate_{std::move(delegate)}, maximum_fragment_{maximum_fragment}
		{
		}

		materialization_io_result<void> append(const std::span<const std::byte> input) override
		{
			return delegate_->append(input);
		}

		materialization_io_result<void> seal() override
		{
			return delegate_->seal();
		}

		materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
		{
			largest_destination_ = std::max(largest_destination_, destination.size());
			return delegate_->read(
				destination.first(std::min(destination.size(), maximum_fragment_)));
		}

		materialization_io_result<std::size_t> read_at(const std::uint64_t offset,
													   std::span<std::byte> destination) override
		{
			largest_destination_ = std::max(largest_destination_, destination.size());
			if (failed_range_ && offset >= failed_range_->first && offset < failed_range_->second)
			{
				++failed_range_reads_;
				if (failed_range_reads_ > permitted_failed_range_reads_)
				{
					++failure_hits_;
					if (failed_range_failure_kind_)
						return materialization_io_failure{*failed_range_failure_kind_,
														  materialization_io_operation::spool_read};
					return std::size_t{};
				}
			}
			return delegate_->read_at(
				offset, destination.first(std::min(destination.size(), maximum_fragment_)));
		}

		materialization_io_result<void> rewind() override
		{
			return delegate_->rewind();
		}

		[[nodiscard]] std::uint64_t size_bytes() const noexcept override
		{
			return delegate_->size_bytes();
		}

		[[nodiscard]] bool sealed() const noexcept override
		{
			return report_unsealed_ ? false : delegate_->sealed();
		}

		std::unique_ptr<materialization_replayable_spool> delegate_;
		std::size_t maximum_fragment_{};
		std::size_t largest_destination_{};
		std::optional<std::pair<std::uint64_t, std::uint64_t>> failed_range_;
		std::optional<materialization_io_failure_kind> failed_range_failure_kind_;
		std::size_t permitted_failed_range_reads_{};
		std::size_t failed_range_reads_{};
		std::size_t failure_hits_{};
		bool report_unsealed_{};
	};

	enum class identity_digest_fault : std::uint8_t
	{
		update_hash,
		update_allocation,
		update_invalid_configuration,
		finish_hash,
		finish_allocation,
		finish_invalid_configuration,
		finish_invalid_pair,
		finish_malformed,
	};

	class faulting_identity_digest final : public materialization_digest_accumulator
	{
	  public:
		explicit faulting_identity_digest(const identity_digest_fault fault)
			: delegate_{make_materialization_sha256_accumulator()}, fault_{fault}
		{
		}

		materialization_io_result<void> update(std::span<const std::byte> input) override
		{
			if (fault_ == identity_digest_fault::update_hash ||
				fault_ == identity_digest_fault::update_allocation ||
				fault_ == identity_digest_fault::update_invalid_configuration)
				return materialization_io_failure{
					fault_ == identity_digest_fault::update_hash
						? materialization_io_failure_kind::hash
						: fault_ == identity_digest_fault::update_allocation
						? materialization_io_failure_kind::allocation
						: materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::digest_update};
			return delegate_->update(input);
		}

		materialization_io_result<std::string> finish() override
		{
			if (fault_ == identity_digest_fault::finish_hash ||
				fault_ == identity_digest_fault::finish_allocation ||
				fault_ == identity_digest_fault::finish_invalid_configuration ||
				fault_ == identity_digest_fault::finish_invalid_pair)
				return materialization_io_failure{
					fault_ == identity_digest_fault::finish_hash
						? materialization_io_failure_kind::hash
						: fault_ == identity_digest_fault::finish_allocation
						? materialization_io_failure_kind::allocation
						: fault_ == identity_digest_fault::finish_invalid_configuration
						? materialization_io_failure_kind::invalid_configuration
						: materialization_io_failure_kind::read,
					materialization_io_operation::digest_finalize};
			if (fault_ == identity_digest_fault::finish_malformed)
				return std::string{"SHA256:not-canonical"};
			return delegate_->finish();
		}

	  private:
		std::unique_ptr<materialization_digest_accumulator> delegate_;
		identity_digest_fault fault_;
	};

	class identity_digest_factory final : public materialization_request_identity_digest_factory
	{
	  public:
		explicit identity_digest_factory(const identity_digest_fault fault) : fault_{fault} {}

		std::unique_ptr<materialization_digest_accumulator> make_digest() override
		{
			return std::make_unique<faulting_identity_digest>(fault_);
		}

	  private:
		identity_digest_fault fault_;
	};

	struct scanned_request
	{
		std::string raw;
		std::unique_ptr<fragmented_spool> spool;
		std::unique_ptr<materialization_request_task_index> task_index;
		materialization_request_envelope envelope;
	};

	[[nodiscard]] std::string member(const std::string_view name, std::string value)
	{
		return "\"" + std::string{name} + "\":" + std::move(value);
	}

	struct request_options
	{
		std::string first_source_token{"\"YQ==\""};
		bool reverse_root{};
		bool reverse_tasks{};
		std::string backend{"\"memory\""};
		std::string series{"\"series:one\""};
		std::string parent{"null"};
		std::string sqlite_path{"null"};
		std::string selector_channel{"\"channel:one\""};
		std::string tool_value{R"({"z":true,"a":null,"number":1e2})"};
	};

	[[nodiscard]] std::string request_text(const request_options& options = request_options{})
	{
		const auto first_task = R"({"z":null,"source":{"encoding":"utf8","content_base64":)" +
			options.first_source_token + R"(},"order":[true,1,-2],"name":"first"})";
		const std::string second_task =
			R"({"name":"second","source":{"content_base64":"Yg==","encoding":"utf8"},"order":[2,1]})";
		const auto tasks = options.reverse_tasks ? "[" + second_task + "," + first_task + "]"
												 : "[" + first_task + "," + second_task + "]";
		const auto publication = R"({"selector":{"channel_id":)" + options.selector_channel +
			R"(,"catalog_id":"catalog:one"},"backend":)" + options.backend +
			",\"series_id\":" + options.series +
			",\"expected_parent_publication\":" + options.parent +
			",\"sqlite_path\":" + options.sqlite_path + R"(,"genesis":true})";
		std::vector<std::string> members{
			member("schema", "\"cxxlens.clang22-materialization-request.v2\""),
			member("request_version", "\"2.1.0\""),
			member("materialization_request_id", "\"pending-materialization-id\""),
			member("request_digest", "\"pending-request-digest\""),
			member("semantic_request_digest", "\"pending-semantic-digest\""),
			member("tool", options.tool_value),
			member("worker", R"({"protocol":1,"enabled":true})"),
			member("project", R"({"id":"project:one"})"),
			member("registry", R"({"descriptors":["b","a"]})"),
			member("engine", R"({"generation":1})"),
			member("interpretation_policy", R"({"domain":"canonical"})"),
			member("trust_policy", R"({"profile":"native"})"),
			member("group_topology", R"({"groups":["canonical","observation"]})"),
			member("tasks", tasks),
			member("publication", publication),
		};
		if (options.reverse_root)
			std::ranges::reverse(members);
		std::string output{"{"};
		for (std::size_t index{}; index < members.size(); ++index)
		{
			if (index != 0U)
				output.push_back(',');
			output += members[index];
		}
		output.push_back('}');
		return output;
	}

	[[nodiscard]] scanned_request scan(std::string raw, const std::size_t fragment = 3U)
	{
		auto storage = make_materialization_private_spool();
		require(storage.has_value(), "private request spool creation failed");
		auto spool = std::make_unique<fragmented_spool>(std::move(*storage), fragment);
		require(spool->append(bytes(raw)).has_value(), "request spool append failed");
		require(spool->seal().has_value(), "request spool seal failed");
		auto index = make_materialization_request_task_index(spool->size_bytes());
		require(index.has_value(), "task index creation failed");
		auto envelope = scan_materialization_request_envelope(*spool, {17U}, index->get());
		require(envelope.has_value(), "v2.1 request envelope scan failed");
		return {std::move(raw), std::move(spool), std::move(*index), std::move(*envelope)};
	}

	[[nodiscard]] streamed_materialization_request_identity oracle(const std::string& raw)
	{
		json_limits limits;
		limits.max_input_bytes = raw.size();
		limits.max_string_bytes = raw.size();
		limits.max_total_string_bytes = raw.size();
		auto parsed = parse_json_object(raw, limits);
		require(parsed.has_value(), "small DOM oracle request did not parse");
		constexpr std::array removed{
			std::string_view{"materialization_request_id"},
			std::string_view{"request_digest"},
			std::string_view{"semantic_request_digest"},
		};
		auto request_projection = object_without(parsed->root(), removed);
		require(request_projection.has_value(), "request oracle projection failed");
		auto request =
			projection_digest("cxxlens.clang22-materialization-request.v2", *request_projection);
		require(request.has_value(), "request oracle digest failed");

		auto semantic_map = *request_projection->as_object();
		auto publication = semantic_map.find("publication");
		require(publication != semantic_map.end() && publication->second.as_object() != nullptr,
				"semantic oracle publication missing");
		auto semantic_publication = *publication->second.as_object();
		for (const auto name :
			 {"backend", "series_id", "expected_parent_publication", "sqlite_path"})
			semantic_publication.erase(name);
		auto publication_value = json_object(std::move(semantic_publication));
		require(publication_value.has_value(), "semantic oracle publication projection failed");
		publication->second = std::move(*publication_value);
		auto semantic_projection = json_object(std::move(semantic_map));
		require(semantic_projection.has_value(), "semantic oracle root projection failed");
		auto semantic =
			projection_digest("cxxlens.clang22-semantic-request.v2", *semantic_projection);
		require(semantic.has_value(), "semantic oracle digest failed");
		return {"materialization:" + *request, std::move(*request), std::move(*semantic)};
	}

	[[nodiscard]] streamed_materialization_request_identity derive(scanned_request& value)
	{
		auto identity = derive_streamed_materialization_request_identity(
			*value.spool, value.envelope, *value.task_index);
		require(identity.has_value(), "streamed request identity derivation failed");
		return std::move(*identity);
	}

	void replace_once(std::string& value, const std::string_view from, const std::string_view to)
	{
		const auto offset = value.find(from);
		require(offset != std::string::npos, "identity fixture replacement target missing");
		value.replace(offset, from.size(), to);
	}

	[[nodiscard]] std::string bind(std::string raw,
								   const streamed_materialization_request_identity& identity)
	{
		replace_once(raw, "pending-materialization-id", identity.materialization_request_id);
		replace_once(raw, "pending-request-digest", identity.request_digest);
		replace_once(raw, "pending-semantic-digest", identity.semantic_request_digest);
		return raw;
	}

	void sdk_oracle_and_canonical_equivalence()
	{
		auto canonical = scan(request_text(), 1U);
		const auto streamed = derive(canonical);
		require(streamed == oracle(canonical.raw),
				"streamed identity diverged from SDK canonical tuple oracle");

		auto reordered = scan(
			request_text({.first_source_token = "\"YQ\\u003d\\u003d\"", .reverse_root = true}), 2U);
		const auto reordered_identity = derive(reordered);
		require(reordered_identity == streamed,
				"UTF-8 key sorting or decoded JSON source spelling changed identity");

		const auto bound_raw = bind(canonical.raw, streamed);
		auto bound = scan(bound_raw, 1U);
		auto validated = validate_streamed_materialization_request_identity(
			*bound.spool, bound.envelope, *bound.task_index);
		require(validated && *validated == streamed,
				"exact supplied request identity was not validated");
	}

	void projection_boundaries_and_mismatch_fields()
	{
		auto base = scan(request_text());
		const auto base_identity = derive(base);
		auto excluded = scan(request_text({.backend = "\"sqlite\"",
										   .series = "\"series:other\"",
										   .parent = "\"publication:parent\"",
										   .sqlite_path = "\"db.sqlite\""}));
		const auto excluded_identity = derive(excluded);
		require(excluded_identity.request_digest != base_identity.request_digest &&
					excluded_identity.semantic_request_digest ==
						base_identity.semantic_request_digest,
				"publication persistence fields crossed the semantic projection boundary");

		auto selector = scan(request_text({.selector_channel = "\"channel:other\""}));
		const auto selector_identity = derive(selector);
		require(selector_identity.request_digest != base_identity.request_digest &&
					selector_identity.semantic_request_digest !=
						base_identity.semantic_request_digest,
				"publication selector was incorrectly removed from semantic identity");

		auto reordered_tasks = scan(request_text({.reverse_tasks = true}));
		require(derive(reordered_tasks).request_digest != base_identity.request_digest,
				"task array order did not remain identity-significant");

		auto wrong_raw = bind(base.raw, base_identity);
		replace_once(wrong_raw,
					 "\"request_digest\":\"" + base_identity.request_digest + "\"",
					 "\"request_digest\":\"semantic-v2:sha256:wrong\"");
		auto wrong = scan(std::move(wrong_raw));
		auto rejected = validate_streamed_materialization_request_identity(
			*wrong.spool, wrong.envelope, *wrong.task_index);
		require(!rejected && rejected.error().code == "materialization.identity-mismatch" &&
					rejected.error().field == "request.request_digest",
				"request digest mismatch lost its exact binding field");

		auto wrong_semantic_raw = bind(base.raw, base_identity);
		replace_once(wrong_semantic_raw,
					 "\"semantic_request_digest\":\"" + base_identity.semantic_request_digest +
						 "\"",
					 "\"semantic_request_digest\":\"semantic-v2:sha256:wrong\"");
		auto wrong_semantic = scan(std::move(wrong_semantic_raw));
		auto semantic_rejected = validate_streamed_materialization_request_identity(
			*wrong_semantic.spool, wrong_semantic.envelope, *wrong_semantic.task_index);
		require(!semantic_rejected &&
					semantic_rejected.error().field == "request.semantic_request_digest",
				"semantic digest mismatch lost its exact binding field");

		auto wrong_id_raw = bind(base.raw, base_identity);
		replace_once(wrong_id_raw,
					 "\"materialization_request_id\":\"" +
						 base_identity.materialization_request_id + "\"",
					 "\"materialization_request_id\":\"materialization:wrong\"");
		auto wrong_id = scan(std::move(wrong_id_raw));
		auto id_rejected = validate_streamed_materialization_request_identity(
			*wrong_id.spool, wrong_id.envelope, *wrong_id.task_index);
		require(!id_rejected && id_rejected.error().field == "request.materialization_request_id",
				"materialization ID mismatch lost its exact binding field");

		auto unsigned_request =
			scan(request_text({.tool_value = R"({"integer":18446744073709551615})"}));
		auto unsigned_rejected = derive_streamed_materialization_request_identity(
			*unsigned_request.spool, unsigned_request.envelope, *unsigned_request.task_index);
		require(!unsigned_rejected &&
					is_materialization_admission_no_response(unsigned_rejected.error()),
				"unsafe canonical signed-domain value escaped the no-response boundary");
	}

	void sealed_short_read_and_large_source()
	{
		auto unsealed = scan(request_text());
		unsealed.spool->report_unsealed_ = true;
		auto mutable_rejected = derive_streamed_materialization_request_identity(
			*unsealed.spool, unsealed.envelope, *unsealed.task_index);
		require(!mutable_rejected, "mutable raw request spool entered request identity");

		auto short_read = scan(request_text(), 1U);
		auto first_task = short_read.task_index->at(0U);
		require(first_task && first_task->source_content_offset &&
					first_task->source_content_size_bytes,
				"source short-read fixture was not indexed");
		const auto source_offset = first_task->source_content_offset.value_or(0U);
		const auto source_size = first_task->source_content_size_bytes.value_or(0U);
		short_read.spool->failed_range_ = std::pair{source_offset, source_offset + source_size};
		short_read.spool->permitted_failed_range_reads_ = 2U;
		auto truncated = derive_streamed_materialization_request_identity(
			*short_read.spool, short_read.envelope, *short_read.task_index);
		require(short_read.spool->failure_hits_ != 0U,
				"source-token short-read injection was not exercised");
		require(!truncated && is_materialization_admission_no_response(truncated.error()),
				"successful source-token truncation escaped the no-response boundary");

		for (const auto kind : {materialization_io_failure_kind::read,
								materialization_io_failure_kind::allocation,
								materialization_io_failure_kind::invalid_configuration})
		{
			auto failed = scan(request_text(), 1U);
			auto task = failed.task_index->at(0U);
			require(task && task->source_content_offset && task->source_content_size_bytes,
					"source I/O failure fixture was not indexed");
			const auto begin = task->source_content_offset.value_or(0U);
			failed.spool->failed_range_ =
				std::pair{begin, begin + task->source_content_size_bytes.value_or(0U)};
			failed.spool->permitted_failed_range_reads_ = 2U;
			failed.spool->failed_range_failure_kind_ = kind;
			auto result = derive_streamed_materialization_request_identity(
				*failed.spool, failed.envelope, *failed.task_index);
			if (kind != materialization_io_failure_kind::read)
				require(!result && is_materialization_admission_no_response(result.error()),
						"identity source allocation escaped the no-response boundary");
			else
				require(!result && result.error().code == "materialization.spool-failure" &&
							result.error().field == "request-binding" &&
							result.error().detail.starts_with("identity:source-content-read:"),
						"identity source I/O lost its request-binding spool taxonomy");
		}

		std::string large_source(8U * 1024U * 1024U + 16U, 'A');
		auto large = scan(request_text({.first_source_token = "\"" + large_source + "\""}), 4096U);
		auto large_identity = derive_streamed_materialization_request_identity(
			*large.spool, large.envelope, *large.task_index);
		require(large_identity.has_value(),
				"source larger than the task DOM string window was not streamed");
		require(large.spool->largest_destination_ <= default_stream_chunk_bytes,
				"large source requested an all-source read window");
	}

	void digest_failure_taxonomy_is_closed()
	{
		for (const auto fault :
			 {identity_digest_fault::update_hash, identity_digest_fault::finish_hash})
		{
			auto request = scan(request_text(), 7U);
			identity_digest_factory factory{fault};
			auto rejected = derive_streamed_materialization_request_identity(
				*request.spool, request.envelope, *request.task_index, factory);
			require(!rejected && rejected.error().code == "materialization.spool-failure" &&
						rejected.error().field == "request-binding" &&
						rejected.error().detail.starts_with("identity:digest-"),
					"actual identity hash failure lost request-binding spool taxonomy");
		}

		for (const auto fault : {identity_digest_fault::update_allocation,
								 identity_digest_fault::update_invalid_configuration,
								 identity_digest_fault::finish_allocation,
								 identity_digest_fault::finish_invalid_configuration,
								 identity_digest_fault::finish_invalid_pair,
								 identity_digest_fault::finish_malformed})
		{
			auto request = scan(request_text(), 7U);
			identity_digest_factory factory{fault};
			auto rejected = derive_streamed_materialization_request_identity(
				*request.spool, request.envelope, *request.task_index, factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"identity digest contradiction escaped the no-response boundary");
		}
	}
} // namespace

int main()
{
	sdk_oracle_and_canonical_equivalence();
	projection_boundaries_and_mismatch_fields();
	sealed_short_read_and_large_source();
	digest_failure_taxonomy_is_closed();
	return 0;
}
