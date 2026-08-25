#include "llvm/clang22/materialization_store_memory_backend.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace cxxlens::detail::clang22::materialization;
	using cxxlens::sdk::result;

	enum class cas_script : std::uint8_t
	{
		pre_effect_rejection,
		post_effect_uncertain,
	};

	class scripted_cas_port final : public bounded_memory_cas_port
	{
	  public:
		explicit scripted_cas_port(const cas_script script) : script_{script} {}

		[[nodiscard]] result<bounded_memory_publication_terminal>
		compare_exchange_once(const std::string_view expected_head,
							  const std::string_view observed_head,
							  effect commit) override
		{
			if (expected_head != observed_head)
				return bounded_memory_publication_terminal::rejected_stale;
			if (script_ == cas_script::pre_effect_rejection)
				return bounded_memory_publication_terminal::rejected_store_failure;
			if (!commit)
				return cxxlens::sdk::unexpected(
					cxxlens::sdk::error{"store.test-port-invalid", "commit", "missing"});
			auto applied = commit();
			if (!applied)
				return cxxlens::sdk::unexpected(std::move(applied.error()));
			return bounded_memory_publication_terminal::publication_outcome_unknown;
		}

	  private:
		cas_script script_;
	};

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
	{
		std::vector<std::byte> output;
		output.reserve(value.size());
		for (const auto byte : value)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	struct expected_stream
	{
		std::vector<bounded_memory_record> records;
		std::size_t index{};

		[[nodiscard]] result<std::optional<bounded_memory_record>> next()
		{
			if (index == records.size())
				return std::optional<bounded_memory_record>{};
			return std::optional<bounded_memory_record>{records[index++]};
		}
	};

	[[nodiscard]] std::vector<bounded_memory_record> fixture_records()
	{
		return {
			{bounded_memory_record_kind::task_result, "0", bytes("task-zero")},
			{bounded_memory_record_kind::projection, "partition/0", bytes("row-zero")},
			{bounded_memory_record_kind::metadata, "coverage/0", bytes("complete")},
			{bounded_memory_record_kind::task_result, "1", bytes("task-one")},
			{bounded_memory_record_kind::projection, "partition/1", bytes("row-one")},
		};
	}

	[[nodiscard]] bounded_memory_backend_session
	prepare(bounded_memory_backend& backend,
			const std::string_view candidate,
			const std::string_view head,
			std::vector<bounded_memory_record>& expected)
	{
		auto session = backend.begin(std::string{candidate}, std::string{head});
		require(session.has_value(), "memory session could not be opened");
		for (const auto& record : fixture_records())
		{
			if (record.kind == bounded_memory_record_kind::task_result)
			{
				auto appended = session->append_task(record.payload);
				require(appended.has_value(), "task result could not be staged");
			}
			else
			{
				auto appended = session->append_record(record);
				require(appended.has_value(), "physical record could not be staged");
			}
			expected.push_back(record);
		}
		auto sealed = session->seal();
		require(sealed.has_value(), "memory payload could not be sealed");
		return std::move(*session);
	}

	void positive_parity_publish_reopen()
	{
		bounded_memory_backend backend;
		std::vector<bounded_memory_record> expected;
		auto session = prepare(backend, "candidate:memory:positive", "genesis", expected);
		const auto expected_size = session.payload_bytes();
		expected_stream stream{std::move(expected)};
		auto parity = session.compare_expected(
			[&stream]
			{
				return stream.next();
			});
		require(parity.has_value(), "positive physical/expected parity failed");
		require(parity->record_count == session.record_count() &&
					parity->payload_bytes == expected_size &&
					parity->maximum_window_bytes <= bounded_memory_backend_max_record_bytes,
				"positive parity receipt lost the bounded census");
		auto terminal = session.publish_once();
		require(terminal.has_value() &&
					*terminal == bounded_memory_publication_terminal::committed_verified,
				"memory publication was not committed and verified");
		require(session.publication().has_value(), "memory publication identity was not retained");
		require(backend.committed_publication_count() == 1U,
				"memory backend committed more than once");
		auto reopened = backend.reopen(session.publication()->publication_id);
		require(reopened.has_value(), "memory publication could not be reopened");
		require(reopened->verify_identity().has_value(), "reopened memory identity did not verify");
		require(reopened->publication() == *session.publication(),
				"reopened publication identity changed");
		require(reopened->final_payload().size() == expected_size, "reopened payload size changed");
		auto cursor = reopened->open_cursor();
		require(cursor.has_value(), "reopened physical cursor could not open");
		std::uint64_t count{};
		for (;;)
		{
			auto next = (*cursor)->next();
			require(next.has_value(), "reopened cursor failed");
			if (!*next)
				break;
			++count;
			require((*cursor)->decoded_window_bytes() <= (*cursor)->maximum_window_bytes(),
					"physical cursor exceeded O(W) window");
		}
		require(count == session.record_count(), "reopened cursor record count changed");
		require(!session.publish_once(), "second publication attempt was accepted");
	}

	void parity_mismatch_is_zero_effect()
	{
		bounded_memory_backend backend;
		std::vector<bounded_memory_record> expected;
		auto session = prepare(backend, "candidate:memory:mismatch", "genesis", expected);
		expected.back().payload = bytes("tampered");
		expected_stream stream{std::move(expected)};
		auto parity = session.compare_expected(
			[&stream]
			{
				return stream.next();
			});
		require(!parity && parity.error().code == "store.memory-parity-mismatch",
				"tampered expected projection was accepted");
		require(backend.committed_publication_count() == 0U,
				"parity failure changed memory publication state");
		auto publish = session.publish_once();
		require(!publish && publish.error().field == "publish",
				"parity failure reached publication or changed error boundary");
	}

	void stale_cas_is_terminal()
	{
		bounded_memory_backend backend;
		std::vector<bounded_memory_record> first_expected;
		auto first = prepare(backend, "candidate:memory:first", "genesis", first_expected);
		expected_stream first_stream{std::move(first_expected)};
		require(first
					.compare_expected(
						[&first_stream]
						{
							return first_stream.next();
						})
					.has_value(),
				"first parity failed");
		auto first_terminal = first.publish_once();
		require(first_terminal.has_value() &&
					*first_terminal == bounded_memory_publication_terminal::committed_verified,
				"first CAS did not commit");

		std::vector<bounded_memory_record> second_expected;
		auto second = prepare(backend, "candidate:memory:second", "genesis", second_expected);
		expected_stream second_stream{std::move(second_expected)};
		require(second
					.compare_expected(
						[&second_stream]
						{
							return second_stream.next();
						})
					.has_value(),
				"second parity failed");
		auto stale = second.publish_once();
		require(stale.has_value() && *stale == bounded_memory_publication_terminal::rejected_stale,
				"stale expected head was not rejected");
		require(backend.committed_publication_count() == 1U,
				"stale CAS changed committed publication count");
		require(!second.publish_once(), "stale session allowed a retry");
	}

	void unknown_after_cas_is_recoverable()
	{
		bounded_memory_backend backend{
			bounded_memory_backend::options{},
			std::make_shared<scripted_cas_port>(cas_script::post_effect_uncertain)};
		std::vector<bounded_memory_record> expected;
		auto session = prepare(backend, "candidate:memory:unknown", "genesis", expected);
		expected_stream stream{std::move(expected)};
		require(session
					.compare_expected(
						[&stream]
						{
							return stream.next();
						})
					.has_value(),
				"unknown-effect parity failed");
		auto terminal = session.publish_once();
		require(terminal.has_value() &&
					*terminal == bounded_memory_publication_terminal::publication_outcome_unknown,
				"post-CAS ambiguity was not classified as unknown");
		require(session.publication().has_value(), "unknown CAS lost its candidate identity");
		auto reopened = backend.reopen(session.publication()->publication_id);
		require(reopened.has_value() && reopened->verify_identity().has_value(),
				"unknown CAS could not be recovered by reopen verification");
		require(backend.committed_publication_count() == 1U,
				"unknown CAS was retried or duplicated");
	}

	void bounds_and_cas_rejection()
	{
		bounded_memory_backend::options options;
		options.limits.max_payload_bytes = 128U;
		options.limits.max_record_bytes = 128U;
		options.limits.max_window_bytes = 128U;
		bounded_memory_backend backend{options};
		auto session = backend.begin("candidate:memory:bounded", "genesis");
		require(session.has_value(), "bounded memory session could not be opened");
		require(!session->append_task(bytes("this payload is deliberately larger than the bound")),
				"oversized task escaped the preallocation bound");
		require(!session->seal(), "empty bounded session sealed");

		bounded_memory_backend rejected_backend{
			bounded_memory_backend::options{},
			std::make_shared<scripted_cas_port>(cas_script::pre_effect_rejection)};
		std::vector<bounded_memory_record> expected;
		auto rejected_session =
			prepare(rejected_backend, "candidate:memory:pre-cas", "genesis", expected);
		expected_stream stream{std::move(expected)};
		require(rejected_session
					.compare_expected(
						[&stream]
						{
							return stream.next();
						})
					.has_value(),
				"pre-CAS rejection parity failed");
		auto terminal = rejected_session.publish_once();
		require(terminal.has_value() &&
					*terminal == bounded_memory_publication_terminal::rejected_store_failure,
				"pre-CAS rejection was not classified without effect");
		require(rejected_backend.committed_publication_count() == 0U,
				"pre-CAS rejection changed memory state");
	}
} // namespace

int main()
{
	positive_parity_publish_reopen();
	parity_mismatch_is_zero_effect();
	stale_cas_is_terminal();
	unknown_after_cas_is_recoverable();
	bounds_and_cas_rejection();
	std::cout << "bounded memory backend: parity, CAS, reopen, and ambiguity tests passed\n";
	return EXIT_SUCCESS;
}
