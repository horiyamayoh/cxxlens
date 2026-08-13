#include "llvm/clang22/materialization_request_stream.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
						 std::size_t maximum_fragment)
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
			return delegate_->read(
				destination.first(std::min(destination.size(), maximum_fragment_)));
		}

		materialization_io_result<std::size_t> read_at(const std::uint64_t offset,
													   std::span<std::byte> destination) override
		{
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
			return delegate_->sealed();
		}

	  private:
		std::unique_ptr<materialization_replayable_spool> delegate_;
		std::size_t maximum_fragment_{};
	};

	enum class index_spool_fault : std::uint8_t
	{
		none,
		append,
		append_allocation,
		append_invalid_configuration,
		seal,
		seal_allocation,
		seal_invalid_configuration,
		read,
		read_allocation,
		read_invalid_configuration,
		corrupt_read,
	};

	class fault_injecting_index_spool final : public materialization_replayable_spool
	{
	  public:
		explicit fault_injecting_index_spool(
			std::unique_ptr<materialization_replayable_spool> delegate)
			: delegate_{std::move(delegate)}
		{
		}

		materialization_io_result<void> append(const std::span<const std::byte> input) override
		{
			if (fault_ == index_spool_fault::append ||
				fault_ == index_spool_fault::append_allocation ||
				fault_ == index_spool_fault::append_invalid_configuration)
				return materialization_io_failure{
					fault_ == index_spool_fault::append_allocation
						? materialization_io_failure_kind::allocation
						: fault_ == index_spool_fault::append_invalid_configuration
						? materialization_io_failure_kind::invalid_configuration
						: materialization_io_failure_kind::spool,
					materialization_io_operation::spool_write};
			return delegate_->append(input);
		}

		materialization_io_result<void> seal() override
		{
			if (fault_ == index_spool_fault::seal || fault_ == index_spool_fault::seal_allocation ||
				fault_ == index_spool_fault::seal_invalid_configuration)
				return materialization_io_failure{
					fault_ == index_spool_fault::seal_allocation
						? materialization_io_failure_kind::allocation
						: fault_ == index_spool_fault::seal_invalid_configuration
						? materialization_io_failure_kind::invalid_configuration
						: materialization_io_failure_kind::spool,
					materialization_io_operation::spool_seal};
			return delegate_->seal();
		}

		materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
		{
			return delegate_->read(destination);
		}

		materialization_io_result<std::size_t> read_at(const std::uint64_t offset,
													   std::span<std::byte> destination) override
		{
			if (fault_ == index_spool_fault::read || fault_ == index_spool_fault::read_allocation ||
				fault_ == index_spool_fault::read_invalid_configuration)
				return materialization_io_failure{
					fault_ == index_spool_fault::read_allocation
						? materialization_io_failure_kind::allocation
						: fault_ == index_spool_fault::read_invalid_configuration
						? materialization_io_failure_kind::invalid_configuration
						: materialization_io_failure_kind::spool,
					materialization_io_operation::spool_read};
			auto read = delegate_->read_at(offset, destination);
			if (read && *read != 0U && fault_ == index_spool_fault::corrupt_read)
				destination.front() = std::byte{0xffU};
			return read;
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
			return delegate_->sealed();
		}

		std::unique_ptr<materialization_replayable_spool> delegate_;
		index_spool_fault fault_{index_spool_fault::none};
	};

	class collecting_source_spool final : public cxxlens::detail::clang22::clang22_task_source_spool
	{
	  public:
		cxxlens::sdk::result<void> append(const std::span<const std::byte> input) override
		{
			if (fail_append_)
				return cxxlens::sdk::unexpected(
					cxxlens::sdk::error{"materialization.spool-failure", "source-test", "append"});
			if (sealed_)
				return cxxlens::sdk::unexpected(
					cxxlens::sdk::error{"provider.frontend-request-invalid", "source", "sealed"});
			bytes_.insert(bytes_.end(), input.begin(), input.end());
			return {};
		}

		cxxlens::sdk::result<cxxlens::detail::clang22::clang22_task_source_receipt> seal() override
		{
			if (fail_seal_)
				return cxxlens::sdk::unexpected(
					cxxlens::sdk::error{"materialization.spool-failure", "source-test", "seal"});
			if (sealed_)
				return cxxlens::sdk::unexpected(
					cxxlens::sdk::error{"provider.frontend-request-invalid", "source", "sealed"});
			sealed_ = true;
			receipt_ = {bytes_.size(), cxxlens::sdk::content_digest(bytes_), "line-index:test"};
			if (drift_seal_state_)
				sealed_ = false;
			return receipt_;
		}

		cxxlens::sdk::result<std::size_t> read_at(const std::uint64_t offset,
												  std::span<std::byte> destination) override
		{
			if (!sealed_ || offset > bytes_.size())
				return cxxlens::sdk::unexpected(
					cxxlens::sdk::error{"provider.frontend-request-invalid", "source", "replay"});
			const auto count = std::min<std::size_t>(destination.size(), bytes_.size() - offset);
			std::ranges::copy(std::span{bytes_}.subspan(static_cast<std::size_t>(offset), count),
							  destination.begin());
			return count;
		}

		[[nodiscard]] std::uint64_t size_bytes() const noexcept override
		{
			return bytes_.size();
		}

		[[nodiscard]] bool sealed() const noexcept override
		{
			return sealed_;
		}

		[[nodiscard]] const cxxlens::detail::clang22::clang22_task_source_receipt&
		receipt() const noexcept override
		{
			return receipt_;
		}

		std::vector<std::byte> bytes_;
		bool sealed_{};
		bool fail_append_{};
		bool fail_seal_{};
		bool drift_seal_state_{};
		cxxlens::detail::clang22::clang22_task_source_receipt receipt_;
	};

	[[nodiscard]] std::unique_ptr<fragmented_spool>
	spool(const std::string_view input, const std::size_t fragment = 3U, const bool seal = true)
	{
		auto created = make_materialization_private_spool();
		require(created.has_value(), "private request spool creation failed");
		auto result = std::make_unique<fragmented_spool>(std::move(*created), fragment);
		require(result->append(bytes(input)).has_value(), "private request spool append failed");
		if (seal)
			require(result->seal().has_value(), "private request spool seal failed");
		return result;
	}

	[[nodiscard]] std::string request_with(const std::string_view suffix = {})
	{
		return std::string{"{\"schema\":\"cxxlens.clang22-materialization-request.v2\","} +
			"\"request_version\":\"2.1.0\"" + std::string{suffix} + "}";
	}

	[[nodiscard]] std::string complete_shape(const std::string_view tasks = "[{}]",
											 const std::string_view tail = {})
	{
		return std::string{"{\"schema\":\"cxxlens.clang22-materialization-request.v2\","} +
			"\"request_version\":\"2.1.0\","
			"\"materialization_request_id\":\"materialization:test\","
			"\"request_digest\":\"semantic-v2:sha256:test\","
			"\"semantic_request_digest\":\"semantic-v2:sha256:test\","
			"\"tool\":{},\"worker\":{},\"project\":{},\"registry\":{},\"engine\":{},"
			"\"interpretation_policy\":{},\"trust_policy\":{},\"group_topology\":{},"
			"\"tasks\":" +
			std::string{tasks} + ",\"publication\":{}" + std::string{tail} + "}";
	}

	void accepted_envelope_and_fragmentation()
	{
		const auto input = request_with(",\"nested\":{\"value\":1.0,\"items\":[true,null,1e2]},"
										"\"unicode\":\"\\ud83d\\ude00\"");
		for (const auto fragment : {1U, 2U, 3U, 7U, 64U, 4096U})
		{
			auto input_spool = spool(input, fragment);
			auto scanned = scan_materialization_request_envelope(*input_spool, {17U});
			require(scanned && scanned->schema() == materialization_request_schema_v2 &&
						scanned->request_version() == materialization_request_version_v2_1 &&
						scanned->scanned_size_bytes() == input.size(),
					"valid envelope changed across arbitrary positive short reads");
			require(scanned->members().size() == 4U && scanned->members()[0U].name == "schema" &&
						scanned->members()[1U].name == "request_version" &&
						scanned->members()[2U].name == "nested" &&
						scanned->members()[3U].name == "unicode",
					"top-level streaming member index lost decoded input order");
			for (const auto& member : scanned->members())
				require(member.value_offset + member.value_size_bytes <= input.size() &&
							!input
								 .substr(static_cast<std::size_t>(member.value_offset),
										 static_cast<std::size_t>(member.value_size_bytes))
								 .empty(),
						"top-level streaming member span escaped exact raw request bytes");
		}

		std::string large(128U * 1024U, 'a');
		auto large_spool = spool(request_with(",\"ignored\":\"" + large + "\""), 11U);
		auto scanned = scan_materialization_request_envelope(*large_spool, {4096U});
		require(scanned && scanned->scanned_size_bytes() == large_spool->size_bytes(),
				"pass one retained or truncated a chunk-crossing value string");
	}

	void duplicate_and_lexical_rejection()
	{
		const std::vector<std::string> invalid{
			request_with(",\"outer\":{\"a\":1,\"\\u0061\":2}"),
			"{\"schema\":\"cxxlens.clang22-materialization-request.v2\","
			"\"\\u0073chema\":\"cxxlens.clang22-materialization-request.v2\","
			"\"request_version\":\"2.1.0\"}",
			request_with() + " {}",
			"[]",
			"\xef\xbb\xbf" + request_with(),
			request_with(",\"bad\":NaN"),
			request_with(",\"bad\":01"),
			request_with(",\"bad\":1.5"),
			request_with(",\"bad\":18446744073709551616"),
			request_with(",\"bad\":-9223372036854775809"),
			request_with(",\"bad\":1e100000000000000000000000000000000000"),
			request_with(",\"bad\":1e-100000000000000000000000000000000000"),
			request_with(",\"bad\":\"\\ud800\""),
		};
		for (const auto& input : invalid)
		{
			auto input_spool = spool(input, 1U);
			auto scanned = scan_materialization_request_envelope(*input_spool, {5U});
			require(!scanned && scanned.error().code == "materialization.request-invalid" &&
						scanned.error().field == "json-decode",
					"strict lexical or duplicate failure escaped pass one");
		}

		std::string invalid_utf8 = request_with(",\"bad\":\"");
		invalid_utf8.push_back(static_cast<char>(0xff));
		invalid_utf8 += "\"}";
		auto invalid_utf8_spool = spool(invalid_utf8, 2U);
		require(!scan_materialization_request_envelope(*invalid_utf8_spool, {3U}),
				"invalid UTF-8 crossed a fragmented string boundary");
	}

	void integer_number_equivalence()
	{
		const std::vector<std::string> accepted{
			"0",
			"-0",
			"1.0",
			"100e-2",
			"1e2",
			"18446744073709551615",
			"-9223372036854775808",
			"0e999999999999999999999999999999999999",
		};
		for (const auto& number : accepted)
		{
			auto input_spool = spool(request_with(",\"number\":" + number), 2U);
			require(scan_materialization_request_envelope(*input_spool, {7U}).has_value(),
					"integer-valued JSON number was rejected by streaming pass one");
		}
	}

	void spool_backed_task_index()
	{
		const auto input =
			request_with(",\"tasks\":[{\"task\":1,\"source\":{\"content_base64\":\"YQ==\"}}, "
						 "{\"task\":2,\"nested\":[0,1]}, {\"task\":3}]");
		auto input_spool = spool(input, 2U);
		auto index = make_materialization_request_task_index(input_spool->size_bytes());
		require(index.has_value(), "private task index creation failed");
		auto scanned = scan_materialization_request_envelope(*input_spool, {7U}, index->get());
		require(scanned && (*index)->sealed() && (*index)->task_count() == 3U,
				"pass one did not seal the exact task census into its private index");
		const std::vector<std::string_view> expected{
			"{\"task\":1,\"source\":{\"content_base64\":\"YQ==\"}}",
			"{\"task\":2,\"nested\":[0,1]}",
			"{\"task\":3}"};
		for (std::uint64_t task{}; task < expected.size(); ++task)
		{
			auto span = (*index)->at(task);
			require(span &&
						input.substr(static_cast<std::size_t>(span->value_offset),
									 static_cast<std::size_t>(span->value_size_bytes)) ==
							expected[static_cast<std::size_t>(task)],
					"spool-backed task span differs from exact raw request occurrence");
			if (task == 0U)
				require(span->source_content_offset && span->source_content_size_bytes &&
							input.substr(static_cast<std::size_t>(*span->source_content_offset),
										 static_cast<std::size_t>(
											 *span->source_content_size_bytes)) == "\"YQ==\"",
						"task index lost the exact nested source content token span");
			else
				require(!span->source_content_offset && !span->source_content_size_bytes,
						"task index fabricated a missing nested source content token");
		}
		require(!(*index)->at(3U), "task index accepted an out-of-census replay");

		const auto invalid = request_with(",\"tasks\":[{\"a\":1,\"a\":2}]");
		auto invalid_spool = spool(invalid, 1U);
		auto invalid_index = make_materialization_request_task_index(invalid_spool->size_bytes());
		require(invalid_index.has_value(), "invalid-path task index creation failed");
		require(
			!scan_materialization_request_envelope(*invalid_spool, {3U}, invalid_index->get()) &&
				!(*invalid_index)->sealed(),
			"failed lexical pass exposed a sealed task index authority");
	}

	void task_index_spool_failures_are_phase_authentic()
	{
		const auto input =
			complete_shape("[{\"source\":{\"content_base64\":\"YQ==\"},\"label\":\"one\"}]");
		auto invalid_create = make_materialization_request_task_index(
			std::unique_ptr<materialization_replayable_spool>{}, input.size());
		require(!invalid_create && is_materialization_admission_no_response(invalid_create.error()),
				"null task-index storage escaped the no-response boundary");
		auto sealed_storage = make_materialization_private_spool();
		require(sealed_storage && (*sealed_storage)->seal(),
				"sealed task-index precondition fixture failed");
		auto sealed_create =
			make_materialization_request_task_index(std::move(*sealed_storage), input.size());
		require(!sealed_create && is_materialization_admission_no_response(sealed_create.error()),
				"presealed task-index storage escaped the no-response boundary");
		auto nonempty_storage = make_materialization_private_spool();
		require(nonempty_storage && (*nonempty_storage)->append(bytes("x")),
				"nonempty task-index precondition fixture failed");
		auto nonempty_create =
			make_materialization_request_task_index(std::move(*nonempty_storage), input.size());
		require(!nonempty_create &&
					is_materialization_admission_no_response(nonempty_create.error()),
				"nonempty task-index storage escaped the no-response boundary");

		const auto scan_with_fault = [&](const index_spool_fault fault)
		{
			auto input_spool = spool(input, 3U);
			auto storage = make_materialization_private_spool();
			require(storage.has_value(), "task-index fault storage creation failed");
			auto injected = std::make_unique<fault_injecting_index_spool>(std::move(*storage));
			injected->fault_ = fault;
			auto index = make_materialization_request_task_index(std::move(injected),
																 input_spool->size_bytes());
			require(index.has_value(), "task-index fault adapter creation failed");
			return scan_materialization_request_envelope(*input_spool, {7U}, index->get());
		};

		auto append_failure = scan_with_fault(index_spool_fault::append);
		require(!append_failure && append_failure.error().code == "materialization.spool-failure" &&
					append_failure.error().field == "json-decode" &&
					append_failure.error().detail.starts_with("task-index:append:"),
				"task-index append fault escaped json-decode taxonomy");
		auto seal_failure = scan_with_fault(index_spool_fault::seal);
		require(!seal_failure && seal_failure.error().code == "materialization.spool-failure" &&
					seal_failure.error().field == "request-schema" &&
					seal_failure.error().detail.starts_with("task-index:seal:"),
				"task-index seal fault escaped request-schema taxonomy");
		for (const auto fault : {index_spool_fault::append_allocation,
								 index_spool_fault::append_invalid_configuration,
								 index_spool_fault::seal_allocation,
								 index_spool_fault::seal_invalid_configuration})
		{
			auto allocation_failure = scan_with_fault(fault);
			require(!allocation_failure &&
						is_materialization_admission_no_response(allocation_failure.error()),
					"task-index append/seal allocation escaped the no-response boundary");
		}

		auto input_spool = spool(input, 5U);
		auto storage = make_materialization_private_spool();
		require(storage.has_value(), "task-index read-fault storage creation failed");
		auto injected = std::make_unique<fault_injecting_index_spool>(std::move(*storage));
		auto* control = injected.get();
		auto index =
			make_materialization_request_task_index(std::move(injected), input_spool->size_bytes());
		require(index.has_value(), "task-index read-fault adapter creation failed");
		auto scanned = scan_materialization_request_envelope(*input_spool, {11U}, index->get());
		require(scanned.has_value(), "task-index read-fault fixture scan failed");
		control->fault_ = index_spool_fault::read;
		auto read_failure = (*index)->at(0U);
		require(!read_failure && read_failure.error().code == "materialization.spool-failure" &&
					read_failure.error().field == "task-index-private" &&
					read_failure.error().detail.starts_with("task-index:read:"),
				"task-index read fault lost its source-private phase marker");
		control->fault_ = index_spool_fault::corrupt_read;
		auto corrupt = (*index)->at(0U);
		require(!corrupt && is_materialization_admission_no_response(corrupt.error()),
				"corrupt compact task-index record escaped the no-response boundary");
		control->fault_ = index_spool_fault::read_allocation;
		auto allocation_read = (*index)->at(0U);
		require(!allocation_read &&
					is_materialization_admission_no_response(allocation_read.error()),
				"task-index read allocation escaped the no-response boundary");
		control->fault_ = index_spool_fault::read_invalid_configuration;
		auto invalid_configuration_read = (*index)->at(0U);
		require(!invalid_configuration_read &&
					is_materialization_admission_no_response(invalid_configuration_read.error()),
				"task-index read configuration drift escaped the no-response boundary");
	}

	void bounded_global_replay()
	{
		std::string task_marker(256U * 1024U, 'q');
		const auto input =
			complete_shape("[{\"source\":{\"content_base64\":\"" + task_marker + "\"}}]");
		auto input_spool = spool(input, 5U);
		auto index = make_materialization_request_task_index(input_spool->size_bytes());
		require(index.has_value(), "global replay task index creation failed");
		auto envelope = scan_materialization_request_envelope(*input_spool, {4096U}, index->get());
		require(envelope && (*index)->task_count() == 1U,
				"complete request shape did not produce one indexed task");
		auto globals = replay_materialization_request_globals(*input_spool, *envelope);
		require(globals && globals->root().member("tasks") != nullptr &&
					globals->root().member("tasks")->as_array() != nullptr &&
					globals->root().member("tasks")->as_array()->empty() &&
					!globals->raw_bytes().contains(task_marker),
				"global DOM retained task occurrence bytes instead of an empty task window");

		auto too_small = replay_materialization_request_globals(*input_spool, *envelope, 64U);
		require(!too_small && is_materialization_admission_no_response(too_small.error()),
				"injected global replay resource limit became schema invalidity");

		std::string inflated_project = "\"project\":{";
		inflated_project.append(4096U, ' ');
		inflated_project.append("\"escaped\":\"");
		for (std::size_t repeat{}; repeat < 1024U; ++repeat)
			inflated_project.append("\\u0061");
		inflated_project.append("\",\"number\":0e");
		inflated_project.append(4096U, '0');
		inflated_project.push_back('}');
		auto inflated_input = complete_shape();
		const auto project_offset = inflated_input.find("\"project\":{}");
		require(project_offset != std::string::npos,
				"global semantic replay fixture project was not found");
		inflated_input.replace(
			project_offset, std::string_view{"\"project\":{}"}.size(), inflated_project);
		auto inflated_spool = spool(inflated_input, 17U);
		auto inflated_envelope = scan_materialization_request_envelope(*inflated_spool, {4096U});
		require(inflated_envelope.has_value(), "inflated global spelling failed lexical scan");
		auto inflated_globals =
			replay_materialization_request_globals(*inflated_spool, *inflated_envelope, 4096U);
		require(inflated_input.size() > 4096U && inflated_globals &&
					inflated_globals->raw_bytes().size() < 4096U &&
					!inflated_globals->raw_bytes().contains("\\u0061") &&
					!inflated_globals->raw_bytes().contains("0e0000") &&
					inflated_globals->raw_bytes().contains("\"number\":0"),
				"global replay retained whitespace, escaped-string, or numeric spelling inflation");

		const auto extra_input = complete_shape("[{}]", ",\"extra\":true");
		auto extra_spool = spool(extra_input);
		auto extra_envelope = scan_materialization_request_envelope(*extra_spool);
		require(extra_envelope.has_value(), "extra-root-member fixture did not scan");
		auto extra_schema = replay_materialization_request_globals(*extra_spool, *extra_envelope);
		require(!extra_schema && extra_schema.error().code == "materialization.request-invalid" &&
					extra_schema.error().field == "request-schema",
				"unknown root member escaped selected-schema invalidity");
		auto extra_binding = replay_materialization_request_globals(
			*extra_spool,
			*extra_envelope,
			maximum_materialization_global_request_window_bytes,
			"request-binding");
		require(!extra_binding && is_materialization_admission_no_response(extra_binding.error()),
				"post-schema extra root member escaped the no-response boundary");

		std::string missing_input = complete_shape();
		const std::string publication = ",\"publication\":{}";
		const auto publication_offset = missing_input.find(publication);
		require(publication_offset != std::string::npos,
				"test request publication member was not found");
		missing_input.erase(publication_offset, publication.size());
		auto missing_spool = spool(missing_input);
		auto missing_envelope = scan_materialization_request_envelope(*missing_spool);
		require(missing_envelope.has_value(), "missing-root-member fixture did not scan");
		auto missing_schema =
			replay_materialization_request_globals(*missing_spool, *missing_envelope);
		require(!missing_schema &&
					missing_schema.error().code == "materialization.request-invalid" &&
					missing_schema.error().field == "request-schema",
				"missing root member escaped selected-schema invalidity");
		auto missing_binding = replay_materialization_request_globals(
			*missing_spool,
			*missing_envelope,
			maximum_materialization_global_request_window_bytes,
			"request-binding");
		require(!missing_binding &&
					is_materialization_admission_no_response(missing_binding.error()),
				"post-schema missing root member escaped the no-response boundary");
	}

	void one_task_metadata_and_source_window()
	{
		std::string task_json =
			"[{\"source\":{\"content_base64\":\"YQ\\u003d\\u003d\"},\"label\":\"kept\",";
		task_json.append(2048U, ' ');
		task_json.append("\"escaped\":\"");
		for (std::size_t index{}; index < 512U; ++index)
			task_json.append("\\u0061");
		task_json.append("\",\"number\":0e");
		task_json.append(2048U, '0');
		task_json.append("}]");
		const auto input = complete_shape(task_json);
		auto input_spool = spool(input, 1U);
		auto index = make_materialization_request_task_index(input_spool->size_bytes());
		require(index.has_value(), "task window index creation failed");
		auto envelope = scan_materialization_request_envelope(*input_spool, {3U}, index->get());
		require(envelope && (*index)->task_count() == 1U,
				"task window request did not retain one exact task span");
		auto task = (*index)->at(0U);
		require(task && task->source_content_offset && task->source_content_size_bytes,
				"task window did not retain source content occurrence");
		auto metadata = replay_materialization_task_metadata(*input_spool, *task);
		const auto* source = metadata ? metadata->root().member("source") : nullptr;
		const auto* content = source != nullptr ? source->member("content_base64") : nullptr;
		require(metadata && content != nullptr && content->as_string() != nullptr &&
					content->as_string()->empty() && metadata->root().member("label") != nullptr &&
					!metadata->raw_bytes().contains("YQ") &&
					!metadata->raw_bytes().contains("\\u0061") &&
					metadata->raw_bytes().contains("\"number\":0"),
				"task metadata replay retained bulk base64 or lost nonbulk metadata");
		auto bounded_metadata = replay_materialization_task_metadata(*input_spool, *task, 2048U);
		require(input.size() > 2048U && bounded_metadata &&
					bounded_metadata->raw_bytes().size() < 2048U,
				"task metadata replay window remained bound to inflated raw spelling");
		collecting_source_spool decoded;
		auto receipt = decode_materialization_task_source(*input_spool, *task, decoded);
		require(receipt && decoded.sealed_ &&
					decoded.bytes_ ==
						std::vector<std::byte>{std::byte{static_cast<unsigned char>('a')}} &&
					receipt->size_bytes == 1U &&
					receipt->content_digest == cxxlens::sdk::content_digest(decoded.bytes_),
				"escaped JSON/base64 source was not decoded directly into its sealed spool");
		auto too_small = replay_materialization_task_metadata(*input_spool, *task, 8U);
		require(!too_small && is_materialization_admission_no_response(too_small.error()),
				"injected task metadata resource limit became schema invalidity");

		for (const auto malformed_task : {std::string_view{R"({"label":"missing-source"})"},
										  std::string_view{R"({"source":{}})"},
										  std::string_view{R"({"source":{"content_base64":{}}})"},
										  std::string_view{R"({"source":{"content_base64":7}})"}})
		{
			const auto malformed_input = complete_shape("[" + std::string{malformed_task} + "]");
			auto malformed_spool = spool(malformed_input, 2U);
			auto malformed_index =
				make_materialization_request_task_index(malformed_spool->size_bytes());
			require(malformed_index.has_value(), "malformed task index creation failed");
			auto malformed_envelope = scan_materialization_request_envelope(
				*malformed_spool, {5U}, malformed_index->get());
			require(malformed_envelope && (*malformed_index)->task_count() == 1U,
					"malformed selected-shape task did not reach replay");
			auto malformed_span = (*malformed_index)->at(0U);
			require(malformed_span.has_value(),
					"malformed selected-shape task span was not indexed");
			auto schema_rejected =
				replay_materialization_task_metadata(*malformed_spool, *malformed_span);
			require(!schema_rejected &&
						schema_rejected.error().code == "materialization.request-invalid" &&
						schema_rejected.error().field == "request-schema",
					"selected task/source/content shape escaped request-schema invalidity");
			auto binding_rejected = replay_materialization_task_metadata(
				*malformed_spool,
				*malformed_span,
				maximum_materialization_task_metadata_window_bytes,
				"request-binding");
			require(!binding_rejected &&
						is_materialization_admission_no_response(binding_rejected.error()),
					"post-schema task/source/content shape drift escaped no-response");
		}

		for (const auto phase :
			 {std::string_view{"request-schema"}, std::string_view{"request-binding"}})
		{
			collecting_source_spool append_failure;
			append_failure.fail_append_ = true;
			auto append_rejected =
				decode_materialization_task_source(*input_spool, *task, append_failure, phase);
			require(!append_rejected &&
						append_rejected.error().code == "materialization.spool-failure" &&
						append_rejected.error().field == phase &&
						append_rejected.error().detail.starts_with("source-spool:append:"),
					"source append fault escaped its exact admission phase");

			collecting_source_spool seal_failure;
			seal_failure.fail_seal_ = true;
			auto seal_rejected =
				decode_materialization_task_source(*input_spool, *task, seal_failure, phase);
			require(!seal_rejected &&
						seal_rejected.error().code == "materialization.spool-failure" &&
						seal_rejected.error().field == phase &&
						seal_rejected.error().detail.starts_with("source-spool:seal:"),
					"source seal fault escaped its exact admission phase");

			collecting_source_spool seal_state_drift;
			seal_state_drift.drift_seal_state_ = true;
			auto drift_rejected =
				decode_materialization_task_source(*input_spool, *task, seal_state_drift, phase);
			require(!drift_rejected &&
						is_materialization_admission_no_response(drift_rejected.error()),
					"successful source seal state drift escaped the no-response boundary");
		}

		for (const auto source_token : {std::string_view{"\"Y===\""},
										std::string_view{"\"YR==\""},
										std::string_view{"\"YQ==\\n\""},
										std::string_view{"{}"}})
		{
			const auto invalid_input = complete_shape(
				"[{\"source\":{\"content_base64\":" + std::string{source_token} + "}}]");
			auto invalid_spool = spool(invalid_input, 2U);
			auto invalid_index =
				make_materialization_request_task_index(invalid_spool->size_bytes());
			require(invalid_index.has_value(), "invalid source index creation failed");
			auto invalid_envelope =
				scan_materialization_request_envelope(*invalid_spool, {5U}, invalid_index->get());
			require(invalid_envelope.has_value(), "invalid source envelope scan failed early");
			auto invalid_task = (*invalid_index)->at(0U);
			require(invalid_task.has_value(), "invalid source task span was not indexed");
			collecting_source_spool rejected;
			auto schema_rejected =
				decode_materialization_task_source(*invalid_spool, *invalid_task, rejected);
			require(!schema_rejected &&
						schema_rejected.error().code == "materialization.request-invalid" &&
						schema_rejected.error().field == "request-schema",
					"invalid or noncanonical base64 source was accepted");
			collecting_source_spool binding_rejected;
			auto invariant = decode_materialization_task_source(
				*invalid_spool, *invalid_task, binding_rejected, "request-binding");
			require(!invariant && is_materialization_admission_no_response(invariant.error()),
					"post-schema source drift escaped the no-response boundary");
		}
	}

	void envelope_and_limit_phases()
	{
		const std::vector<std::string> malformed_envelopes{
			"{\"request_version\":\"2.1.0\"}",
			"{\"schema\":{},\"request_version\":\"2.1.0\"}",
			"{\"schema\":\"wrong\",\"request_version\":\"2.1.0\"}",
		};
		for (const auto& input : malformed_envelopes)
		{
			auto input_spool = spool(input);
			auto scanned = scan_materialization_request_envelope(*input_spool);
			require(!scanned && scanned.error().field == "request-envelope" &&
						scanned.error().code == "materialization.request-invalid",
					"malformed envelope was assigned to the wrong phase");
		}

		auto version_spool = spool("{\"schema\":\"cxxlens.clang22-materialization-request.v2\","
								   "\"request_version\":\"2.2.0\"}");
		auto version = scan_materialization_request_envelope(*version_spool);
		require(!version && version.error().field == "request-version" &&
					version.error().code == "materialization.version-unsupported",
				"unsupported adjacent version did not fail at version dispatch");

		const std::string long_suffix(256U * 1024U, 'x');
		auto long_schema_spool = spool("{\"schema\":\"cxxlens.clang22-materialization-request.v2" +
										   long_suffix + "\",\"request_version\":\"2.1.0\"}",
									   13U);
		auto long_schema = scan_materialization_request_envelope(*long_schema_spool, {17U});
		require(!long_schema && long_schema.error().field == "request-envelope",
				"bounded schema capture changed unsupported-schema precedence");

		auto long_version_spool =
			spool("{\"schema\":\"cxxlens.clang22-materialization-request.v2\","
				  "\"request_version\":\"2.1.0" +
					  long_suffix + "\"}",
				  11U);
		auto long_version = scan_materialization_request_envelope(*long_version_spool, {19U});
		require(!long_version && long_version.error().field == "request-version" &&
					long_version.error().code == "materialization.version-unsupported",
				"bounded version capture changed unsupported-version precedence");

		auto malformed_after_long_schema =
			spool("{\"schema\":\"cxxlens.clang22-materialization-request.v2" + long_suffix +
					  "\",\"request_version\":\"2.1.0\",\"tail\":]}",
				  7U);
		auto malformed = scan_materialization_request_envelope(*malformed_after_long_schema, {23U});
		require(!malformed && malformed.error().field == "json-decode",
				"bounded envelope capture masked a later lexical failure");

		auto escaped_schema_spool =
			spool("{\"schema\":\"cxxlens.clang22-materialization-request.v\\u0032\","
				  "\"request_version\":\"2.1.0\"}",
				  1U);
		auto escaped_schema = scan_materialization_request_envelope(*escaped_schema_spool, {3U});
		require(escaped_schema && escaped_schema->schema() == materialization_request_schema_v2,
				"bounded envelope capture rejected an exact escaped schema literal");

		auto array_spool = spool(request_with(",\"values\":[0,1,2]"));
		materialization_request_scan_limits array_limit;
		array_limit.maximum_elements_per_array = 2U;
		auto invalid_array_limit = scan_materialization_request_envelope(*array_spool, array_limit);
		require(!invalid_array_limit &&
					is_materialization_admission_no_response(invalid_array_limit.error()),
				"injected array window limit was ignored");

		auto depth_spool = spool(request_with(",\"value\":{\"nested\":{}}"));
		materialization_request_scan_limits depth_limit;
		depth_limit.maximum_depth = 2U;
		auto invalid_depth_limit = scan_materialization_request_envelope(*depth_spool, depth_limit);
		require(!invalid_depth_limit &&
					is_materialization_admission_no_response(invalid_depth_limit.error()),
				"injected depth limit was ignored");

		auto unsealed = spool(request_with(), 3U, false);
		auto invalid_unsealed = scan_materialization_request_envelope(*unsealed);
		require(!invalid_unsealed &&
					is_materialization_admission_no_response(invalid_unsealed.error()),
				"mutable raw request spool was accepted as pass-one authority");
	}

	void schema_owned_array_limits_follow_complete_lexical_scan()
	{
		std::string generic_values{"["};
		std::string tasks{"["};
		for (std::size_t index{}; index < 4097U; ++index)
		{
			if (index != 0U)
			{
				generic_values.push_back(',');
				tasks.push_back(',');
			}
			generic_values.push_back('0');
			tasks += "{}";
		}
		generic_values.push_back(']');
		tasks.push_back(']');

		auto generic = spool(request_with(",\"values\":" + generic_values), 41U);
		require(scan_materialization_request_envelope(*generic).has_value(),
				"lexical pass rejected a schema-owned 4097-item value before schema validation");

		const auto oversized_tasks = complete_shape(tasks);
		auto request = spool(oversized_tasks, 43U);
		auto index = make_materialization_request_task_index(request->size_bytes());
		require(index.has_value(), "oversized task-index setup failed");
		auto rejected = scan_materialization_request_envelope(*request, {}, index->get());
		require(!rejected && rejected.error().field == "request-schema" &&
					rejected.error().detail.starts_with("tasks-maxItems:"),
				"top-level tasks maxItems was not retained as selected-schema authority");

		auto wrong_schema_request = oversized_tasks;
		const auto schema_literal = std::string{materialization_request_schema_v2};
		const auto schema_offset = wrong_schema_request.find(schema_literal);
		require(schema_offset != std::string::npos, "oversized schema fixture target missing");
		wrong_schema_request.replace(schema_offset, schema_literal.size(), "unsupported-schema");
		auto wrong_schema_spool = spool(wrong_schema_request, 31U);
		auto wrong_schema_index =
			make_materialization_request_task_index(wrong_schema_spool->size_bytes());
		require(wrong_schema_index.has_value(), "oversized wrong-schema index setup failed");
		auto wrong_schema = scan_materialization_request_envelope(
			*wrong_schema_spool, {}, wrong_schema_index->get());
		require(!wrong_schema && wrong_schema.error().field == "request-envelope",
				"tasks maxItems masked unsupported-schema dispatch after lexical completion");

		auto malformed = spool(complete_shape(tasks, ",\"malformed\":]"), 47U);
		auto malformed_index = make_materialization_request_task_index(malformed->size_bytes());
		require(malformed_index.has_value(), "malformed oversized task-index setup failed");
		auto lexical =
			scan_materialization_request_envelope(*malformed, {}, malformed_index->get());
		require(!lexical && lexical.error().field == "json-decode",
				"tasks maxItems masked a later lexical failure before complete scan");
	}
} // namespace

int main()
{
	accepted_envelope_and_fragmentation();
	duplicate_and_lexical_rejection();
	integer_number_equivalence();
	spool_backed_task_index();
	task_index_spool_failures_are_phase_authentic();
	bounded_global_replay();
	one_task_metadata_and_source_window();
	envelope_and_limit_phases();
	schema_owned_array_limits_follow_complete_lexical_scan();
	return 0;
}
