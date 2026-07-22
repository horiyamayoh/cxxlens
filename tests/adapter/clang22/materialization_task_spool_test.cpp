#include "llvm/clang22/materialization_task_spool.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>
#include <cxxlens/sdk/provider.hpp>

#include "llvm/clang22/materialization_admission_error.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;
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

	[[nodiscard]] std::string base64_encode(const std::span<const std::byte> input)
	{
		constexpr std::string_view alphabet{
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
		std::string output;
		output.reserve(((input.size() + 2U) / 3U) * 4U);
		for (std::size_t offset{}; offset < input.size(); offset += 3U)
		{
			const auto count = std::min<std::size_t>(3U, input.size() - offset);
			std::uint32_t word =
				static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset])) << 16U;
			if (count > 1U)
				word |=
					static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + 1U]))
					<< 8U;
			if (count > 2U)
				word |= std::to_integer<std::uint8_t>(input[offset + 2U]);
			output.push_back(alphabet[(word >> 18U) & 0x3fU]);
			output.push_back(alphabet[(word >> 12U) & 0x3fU]);
			output.push_back(count > 1U ? alphabet[(word >> 6U) & 0x3fU] : '=');
			output.push_back(count > 2U ? alphabet[word & 0x3fU] : '=');
		}
		return output;
	}

	[[nodiscard]] clang22_task_source_receipt
	source_receipt_oracle(const std::span<const std::byte> input)
	{
		std::vector<sdk::canonical_value> offsets{sdk::canonical_value::from_integer(0)};
		for (std::size_t index{}; index < input.size(); ++index)
			if (input[index] == std::byte{'\n'})
				offsets.push_back(
					sdk::canonical_value::from_integer(static_cast<std::int64_t>(index + 1U)));
		const auto content_digest = sdk::content_digest(input);
		const std::array projection{
			sdk::canonical_value::from_string("cxxlens.byte-line-index.v1"),
			sdk::canonical_value::from_string(content_digest),
			sdk::canonical_value::from_integer(static_cast<std::int64_t>(input.size())),
			sdk::canonical_value::from_tuple(std::move(offsets)),
		};
		auto line_index = sdk::canonical_identity_digest("line-index", projection);
		require(line_index.has_value(), "line-index oracle construction failed");
		return {input.size(), content_digest, std::move(*line_index)};
	}

	class controlled_spool final : public materialization_replayable_spool
	{
	  public:
		explicit controlled_spool(
			const std::size_t maximum_fragment = std::numeric_limits<std::size_t>::max(),
			const bool retain_bytes = true)
			: maximum_fragment_{maximum_fragment}, retain_bytes_{retain_bytes}
		{
		}

		materialization_io_result<void> append(const std::span<const std::byte> input) override
		{
			if (throw_append_)
				throw std::bad_alloc{};
			if (allocate_append_)
				return materialization_io_failure{materialization_io_failure_kind::allocation,
												  materialization_io_operation::spool_write};
			if (invalid_append_)
				return materialization_io_failure{
					materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::spool_write};
			if (fail_append_ || sealed_)
				return materialization_io_failure{materialization_io_failure_kind::spool,
												  materialization_io_operation::spool_write};
			if (retain_bytes_)
				value_.insert(value_.end(), input.begin(), input.end());
			size_ += static_cast<std::uint64_t>(input.size());
			return {};
		}

		materialization_io_result<void> seal() override
		{
			if (throw_seal_)
				throw std::bad_alloc{};
			if (allocate_seal_)
				return materialization_io_failure{materialization_io_failure_kind::allocation,
												  materialization_io_operation::spool_seal};
			if (invalid_seal_)
				return materialization_io_failure{
					materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::spool_seal};
			if (fail_seal_)
				return materialization_io_failure{materialization_io_failure_kind::spool,
												  materialization_io_operation::spool_seal};
			sealed_ = true;
			cursor_ = 0U;
			return {};
		}

		materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
		{
			auto read = read_at(cursor_, destination);
			if (read)
				cursor_ += static_cast<std::uint64_t>(*read);
			return read;
		}

		materialization_io_result<std::size_t> read_at(const std::uint64_t offset,
													   std::span<std::byte> destination) override
		{
			if (throw_read_)
				throw std::bad_alloc{};
			if (allocate_read_)
				return materialization_io_failure{materialization_io_failure_kind::allocation,
												  materialization_io_operation::spool_read};
			if (invalid_read_)
				return materialization_io_failure{
					materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::spool_read};
			if (fail_read_)
				return materialization_io_failure{materialization_io_failure_kind::read,
												  materialization_io_operation::spool_read};
			if (overreport_read_)
				return destination.size() + 1U;
			if (zero_read_ && offset < size_)
				return std::size_t{};
			if (!retain_bytes_ || offset > value_.size())
				return materialization_io_failure{materialization_io_failure_kind::read,
												  materialization_io_operation::spool_read};
			const auto count = std::min({destination.size(),
										 maximum_fragment_,
										 value_.size() - static_cast<std::size_t>(offset)});
			std::ranges::copy(std::span{value_}.subspan(static_cast<std::size_t>(offset), count),
							  destination.begin());
			if (corrupt_read_ && count != 0U)
				destination.front() ^= std::byte{0x01U};
			return count;
		}

		materialization_io_result<void> rewind() override
		{
			cursor_ = 0U;
			return {};
		}

		[[nodiscard]] std::uint64_t size_bytes() const noexcept override
		{
			return size_ + size_drift_;
		}

		[[nodiscard]] bool sealed() const noexcept override
		{
			return sealed_ && !report_unsealed_;
		}

		std::vector<std::byte> value_;
		std::uint64_t size_{};
		std::uint64_t size_drift_{};
		std::uint64_t cursor_{};
		std::size_t maximum_fragment_{};
		bool retain_bytes_{};
		bool sealed_{};
		bool fail_append_{};
		bool fail_seal_{};
		bool fail_read_{};
		bool zero_read_{};
		bool overreport_read_{};
		bool corrupt_read_{};
		bool report_unsealed_{};
		bool throw_append_{};
		bool throw_seal_{};
		bool throw_read_{};
		bool allocate_append_{};
		bool allocate_seal_{};
		bool allocate_read_{};
		bool invalid_append_{};
		bool invalid_seal_{};
		bool invalid_read_{};
	};

	class failing_digest final : public materialization_digest_accumulator
	{
	  public:
		explicit failing_digest(
			const bool fail_update = true,
			const materialization_io_failure_kind kind = materialization_io_failure_kind::hash)
			: fail_update_{fail_update}, kind_{kind}
		{
		}

		materialization_io_result<void> update(std::span<const std::byte>) override
		{
			if (!fail_update_)
				return {};
			return materialization_io_failure{kind_, materialization_io_operation::digest_update};
		}

		materialization_io_result<std::string> finish() override
		{
			return materialization_io_failure{kind_, materialization_io_operation::digest_finalize};
		}

	  private:
		bool fail_update_{};
		materialization_io_failure_kind kind_{};
	};

	class invalid_finishing_digest final : public materialization_digest_accumulator
	{
	  public:
		materialization_io_result<void> update(std::span<const std::byte>) override
		{
			return {};
		}

		materialization_io_result<std::string> finish() override
		{
			return std::string{"invalid-digest"};
		}
	};

	[[nodiscard]] std::vector<std::byte> replay_all(clang22_task_source_replay& replay,
													const std::size_t chunk_bytes)
	{
		std::vector<std::byte> output;
		std::vector<std::byte> buffer(chunk_bytes);
		std::uint64_t offset{};
		while (offset < replay.size_bytes())
		{
			auto read = replay.read_at(offset, buffer);
			require(read.has_value() && *read != 0U && *read <= buffer.size(),
					"source replay failed before EOF");
			const auto received = std::span{buffer}.first(*read);
			output.insert(output.end(), received.begin(), received.end());
			offset += static_cast<std::uint64_t>(*read);
		}
		return output;
	}

	[[nodiscard]] std::vector<std::byte> replay_all(clang22_task_input_replay& replay,
													const std::size_t chunk_bytes)
	{
		std::vector<std::byte> output;
		std::vector<std::byte> buffer(chunk_bytes);
		std::uint64_t offset{};
		while (offset < replay.size_bytes())
		{
			auto read = replay.read_at(offset, buffer);
			require(read.has_value() && *read != 0U && *read <= buffer.size(),
					"task-input replay failed before EOF");
			const auto received = std::span{buffer}.first(*read);
			output.insert(output.end(), received.begin(), received.end());
			offset += static_cast<std::uint64_t>(*read);
		}
		return output;
	}

	[[nodiscard]] clang22_task_input fixture_task()
	{
		const std::string source{"int main() { return 0; }\n"};
		auto catalog = sdk::project_catalog::make(
			"project://fixture",
			"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
			{{"catalog-unit:0000",
			  "semantic-v2:sha256:dd5bdb2f9fd85376546c2f486a1ac3ebeed4bdb922351f3c2f4a7bf89be94acb",
			  "sha256:deac66ccb79f6d31c0fa7d358de48e083c15c02ff50ec1ebd4b64314b9e6e196",
			  "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"},
			 {"catalog-unit:0001",
			  "semantic-v2:sha256:68f779154ad8159b42f2ccc79b7e74999742e0c05a05563421e50d0cae028c09",
			  "sha256:76af64be58a1f67608cb4c34771305ad773b173cc4cde76261d749928ad4ea49",
			  "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}});
		require(catalog.has_value(), "fixture project catalog is invalid");

		clang22_task_input output;
		output.project_catalog = std::move(*catalog);
		output.selected_catalog_compile_unit = "catalog-unit:0000";
		output.compile_unit =
			"compile-unit:sha256:be42bfee446b271dd490ce3477e4c2f74e8a6125a6f3ec8a03bbcbe349161e99";
		output.project =
			"project:sha256:9b8cdb2a5afab245af006c61b1bbf0a758687ed969b42d349caf98bcdb6f01c3";
		output.variant =
			"build-variant:sha256:d0d2c433d8c558923be73e7655f2faa65ea94e330c9aa722d0e7d831d6907e01";
		output.toolchain_context =
			"toolchain-context:sha256:"
			"78f64803fb0f0f1ab7f10321ebc90aa52aafb99705407b92e005ff7d6ae82b9a";
		output.toolchain_digest =
			"semantic-v2:sha256:d84b82c787577126d2fbbc4e19f1608f77d1725216cf7647c6ace444d1917dbb";
		output.toolchain = {
			"clang",
			"22.0.0",
			"x86_64-unknown-linux-gnu",
			"sha256:3333333333333333333333333333333333333333333333333333333333333333",
			std::nullopt,
			"sha256:4444444444444444444444444444444444444444444444444444444444444444",
			"sha256:5555555555555555555555555555555555555555555555555555555555555555",
		};
		output.variant_authority = {
			"cxx",
			"cxx23",
			"x86_64-unknown-linux-gnu",
			"sha256:6666666666666666666666666666666666666666666666666666666666666666",
			"sha256:7777777777777777777777777777777777777777777777777777777777777777",
			"sha256:8888888888888888888888888888888888888888888888888888888888888888",
		};
		output.normalized_invocation_digest =
			"semantic-v2:sha256:dd5bdb2f9fd85376546c2f486a1ac3ebeed4bdb922351f3c2f4a7bf89be94acb";
		output.environment_digest =
			"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
		output.language = "cxx";
		output.working_directory = "project://fixture";
		output.condition_universe = "condition-universe:one";
		output.condition = "condition:all";
		output.interpretation = "cc.clang22-canonical-1";
		output.source_snapshot = "source-snapshot:sha256:"
								 "cb28d4d99af02e2bf0d1efc7288f211f595ef0a0caeaed889b66cb0fe995086d";
		output.file =
			"file:sha256:83e065cbf0d8f742fe73a01155b02057c0de0fbe747f88b35ea5e96efe8faf06";
		output.logical_path = "project://main.cpp";
		output.source_content_digest =
			"sha256:deac66ccb79f6d31c0fa7d358de48e083c15c02ff50ec1ebd4b64314b9e6e196";
		output.source_content_base64 = base64_encode(bytes(source));
		output.source_size_bytes = source.size();
		output.source_encoding = "utf8";
		output.line_index =
			"line-index:sha256:99cec457c4ced432a4db1dbb3c30bc291044469abf025737b306ccc7980a3510";
		output.source_read_only = false;
		output.source = source;
		output.arguments = {"clang++", "-std=c++23", "project://main.cpp"};
		output.requested_descriptors = {
			"cc.call_direct_target.v1",
			"cc.call_site.v1",
			"cc.entity.v1",
			"frontend.clang22.call_observation.v2",
			"frontend.clang22.entity_observation.v2",
			"frontend.clang22.type_observation.v2",
		};
		output.dependency_groups = {"canonical", "observation"};
		output.budget.wall_ms = 10000U;
		output.budget.cpu_ms = 10000U;
		output.budget.address_space_bytes = 1073741824U;
		output.budget.transport_bytes = 2097152U;
		output.budget.output_bytes = 1048576U;
		output.budget.rows = 1024U;
		output.budget.diagnostics = 128U;
		output.budget.open_files = 64U;
		output.budget.subprocesses = 1U;
		output.sandbox = {
			"enforced",
			"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
		};
		require(output.validate().has_value(), "exact task.v3 fixture is invalid");
		return output;
	}

	void source_receipt_and_short_replay()
	{
		auto empty = make_materialization_task_source_spool();
		require(empty.has_value(), "empty source spool construction failed");
		auto empty_receipt = (*empty)->seal();
		require(empty_receipt &&
					*empty_receipt == source_receipt_oracle(std::span<const std::byte>{}) &&
					(*empty)->sealed(),
				"empty source lost its zero-offset line-index authority");

		const std::string source{"alpha\nbeta\n"};
		auto controlled = std::make_unique<controlled_spool>(1U);
		auto spool = make_materialization_task_source_spool(
			std::move(controlled), make_materialization_sha256_accumulator());
		require(spool.has_value(), "source spool construction failed");
		for (const auto fragment : {std::string_view{"al"},
									std::string_view{"pha\n"},
									std::string_view{"b"},
									std::string_view{"eta\n"}})
			require((*spool)->append(bytes(fragment)).has_value(),
					"fragmented source append failed");
		auto sealed = (*spool)->seal();
		require(sealed && *sealed == source_receipt_oracle(bytes(source)) &&
					(*spool)->receipt() == *sealed && (*spool)->sealed(),
				"source seal changed content or LF line-index authority");
		require(replay_all(**spool, 7U) ==
					std::vector<std::byte>{bytes(source).begin(), bytes(source).end()},
				"arbitrary positive source short reads changed replay bytes");
		auto sealed_append = (*spool)->append(bytes("x"));
		auto duplicate_seal = (*spool)->seal();
		require(!sealed_append && is_materialization_admission_no_response(sealed_append.error()) &&
					!duplicate_seal &&
					is_materialization_admission_no_response(duplicate_seal.error()) &&
					(*spool)->sealed(),
				"sealed source lifecycle drift escaped the no-response boundary");
		std::array<std::byte, 1U> byte{};
		auto eof = (*spool)->read_at((*spool)->size_bytes(), byte);
		require(eof && *eof == 0U, "source EOF replay was not stable");

		std::string width_boundary(300U, 'x');
		for (const auto index : {0U, 254U, 255U, 256U, 299U})
			width_boundary[index] = '\n';
		auto width_storage = std::make_unique<controlled_spool>(3U);
		auto width_spool = make_materialization_task_source_spool(
			std::move(width_storage), make_materialization_sha256_accumulator());
		require(width_spool && (*width_spool)->append(bytes(width_boundary)),
				"line-offset width-boundary append failed");
		auto width_receipt = (*width_spool)->seal();
		require(width_receipt && *width_receipt == source_receipt_oracle(bytes(width_boundary)),
				"streamed line-index framing changed across the 255/256 byte width boundary");
	}

	void source_limits_and_failures()
	{
		std::vector<std::byte> chunk(1024U * 1024U, std::byte{'x'});
		auto counting =
			std::make_unique<controlled_spool>(std::numeric_limits<std::size_t>::max(), false);
		auto bounded = make_materialization_task_source_spool(
			std::move(counting), make_materialization_sha256_accumulator());
		require(bounded.has_value(), "bounded source spool construction failed");
		for (std::size_t index{}; index < 16U; ++index)
			require((*bounded)->append(chunk).has_value(),
					"exact 16 MiB source boundary was rejected");
		auto source_over_limit = (*bounded)->append(std::span{chunk}.first(1U));
		require((*bounded)->size_bytes() == maximum_clang22_task_source_bytes &&
					!source_over_limit &&
					is_materialization_admission_no_response(source_over_limit.error()) &&
					!(*bounded)->seal() && !(*bounded)->sealed(),
				"source limit+1 did not poison the private spool");

		{
			auto storage = std::make_unique<controlled_spool>();
			storage->throw_append_ = true;
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			auto result = (*failed)->append(bytes("x"));
			require(failed && !result && is_materialization_admission_no_response(result.error()),
					"source append bad_alloc escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			require(failed && (*failed)->append(bytes("x")),
					"source seal bad_alloc fixture append failed");
			injected->throw_seal_ = true;
			auto result = (*failed)->seal();
			require(!result && is_materialization_admission_no_response(result.error()),
					"source seal bad_alloc escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			require(failed && (*failed)->append(bytes("x")) && (*failed)->seal(),
					"source replay bad_alloc fixture setup failed");
			injected->throw_read_ = true;
			std::array<std::byte, 1U> output{};
			auto result = (*failed)->read_at(0U, output);
			require(!result && is_materialization_admission_no_response(result.error()),
					"source replay bad_alloc escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			storage->allocate_append_ = true;
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			auto result = (*failed)->append(bytes("x"));
			require(!result && is_materialization_admission_no_response(result.error()),
					"source append allocation result escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			storage->invalid_append_ = true;
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			auto result = (*failed)->append(bytes("x"));
			require(!result && is_materialization_admission_no_response(result.error()),
					"source append configuration drift escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			require((*failed)->append(bytes("x")).has_value(),
					"source allocation seal fixture append failed");
			injected->allocate_seal_ = true;
			auto result = (*failed)->seal();
			require(!result && is_materialization_admission_no_response(result.error()),
					"source seal allocation result escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			require((*failed)->append(bytes("x")).has_value(),
					"source configuration seal fixture append failed");
			injected->invalid_seal_ = true;
			auto result = (*failed)->seal();
			require(!result && is_materialization_admission_no_response(result.error()),
					"source seal configuration drift escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			require((*failed)->append(bytes("x")) && (*failed)->seal(),
					"source allocation replay fixture setup failed");
			injected->allocate_read_ = true;
			std::array<std::byte, 1U> output{};
			auto result = (*failed)->read_at(0U, output);
			require(!result && is_materialization_admission_no_response(result.error()),
					"source read allocation result escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			require((*failed)->append(bytes("x")) && (*failed)->seal(),
					"source configuration replay fixture setup failed");
			injected->invalid_read_ = true;
			std::array<std::byte, 1U> output{};
			auto result = (*failed)->read_at(0U, output);
			require(!result && is_materialization_admission_no_response(result.error()),
					"source read configuration drift escaped the no-response boundary");
		}
		{
			auto failed = make_materialization_task_source_spool(
				std::make_unique<controlled_spool>(),
				std::make_unique<failing_digest>(true,
												 materialization_io_failure_kind::allocation));
			auto result = (*failed)->append(bytes("x"));
			require(!result && is_materialization_admission_no_response(result.error()),
					"source digest-update allocation escaped the no-response boundary");
		}
		{
			auto failed = make_materialization_task_source_spool(
				std::make_unique<controlled_spool>(),
				std::make_unique<failing_digest>(false,
												 materialization_io_failure_kind::allocation));
			require((*failed)->append(bytes("x")).has_value(),
					"source digest-finalize allocation fixture append failed");
			auto result = (*failed)->seal();
			require(!result && is_materialization_admission_no_response(result.error()),
					"source digest-finalize allocation escaped the no-response boundary");
		}
		for (const auto fail_update : {true, false})
		{
			auto failed = make_materialization_task_source_spool(
				std::make_unique<controlled_spool>(),
				std::make_unique<failing_digest>(
					fail_update, materialization_io_failure_kind::invalid_configuration));
			if (fail_update)
			{
				auto result = (*failed)->append(bytes("x"));
				require(!result && is_materialization_admission_no_response(result.error()),
						"source digest update configuration drift escaped no-response");
			}
			else
			{
				require((*failed)->append(bytes("x")).has_value(),
						"source digest finalize configuration fixture append failed");
				auto result = (*failed)->seal();
				require(!result && is_materialization_admission_no_response(result.error()),
						"source digest finalize configuration drift escaped no-response");
			}
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			injected->size_drift_ = 1U;
			auto result = (*failed)->append(bytes("x"));
			require(!result && is_materialization_admission_no_response(result.error()),
					"source append size drift escaped the no-response boundary");
		}
		for (const auto corruption : {0U, 1U, 2U})
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			require((*failed)->append(bytes("x\n")).has_value(),
					"source successful-corruption fixture append failed");
			if (corruption == 0U)
				injected->report_unsealed_ = true;
			else if (corruption == 1U)
				injected->zero_read_ = true;
			else
				injected->corrupt_read_ = true;
			auto result = (*failed)->seal();
			require(!result && is_materialization_admission_no_response(result.error()),
					"source successful-I/O invariant drift escaped the no-response boundary");
		}
		{
			auto failed = make_materialization_task_source_spool(
				std::make_unique<controlled_spool>(), std::make_unique<invalid_finishing_digest>());
			require((*failed)->append(bytes("x")).has_value(),
					"source invalid-digest fixture append failed");
			auto result = (*failed)->seal();
			require(!result && is_materialization_admission_no_response(result.error()),
					"successful invalid content digest escaped the no-response boundary");
		}

		{
			auto storage = std::make_unique<controlled_spool>();
			storage->fail_append_ = true;
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			auto result = (*failed)->append(bytes("x"));
			require(failed && !result && result.error().code == "materialization.spool-failure" &&
						!(*failed)->seal() && !(*failed)->sealed(),
					"source append failure did not fail closed");
		}
		{
			auto failed = make_materialization_task_source_spool(
				std::make_unique<controlled_spool>(), std::make_unique<failing_digest>());
			require(failed && !(*failed)->append(bytes("x")) && !(*failed)->seal(),
					"source digest failure did not poison the spool");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			storage->fail_seal_ = true;
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			require(failed && (*failed)->append(bytes("x")) && !(*failed)->seal() &&
						!(*failed)->sealed(),
					"source seal failure exposed replay authority");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_source_spool(
				std::move(storage), make_materialization_sha256_accumulator());
			require(failed && (*failed)->append(bytes("x\n")),
					"source read-failure fixture append failed");
			injected->fail_read_ = true;
			require(!(*failed)->seal() && !(*failed)->sealed(),
					"line-index read failure exposed a sealed source receipt");
		}
		require(!make_materialization_task_source_spool(
					std::unique_ptr<materialization_replayable_spool>{},
					make_materialization_sha256_accumulator()) &&
					!make_materialization_task_source_spool(
						std::make_unique<controlled_spool>(),
						std::unique_ptr<materialization_digest_accumulator>{}),
				"null source dependencies were accepted");
	}

	void task_v3_spool_round_trip()
	{
		auto task = fixture_task();
		const auto source_bytes = bytes(task.source);
		auto expected = encode_task_input(task);
		require(expected.has_value(), "one-shot task.v3 oracle encoding failed");

		auto source_storage = std::make_unique<controlled_spool>(2U);
		auto source = make_materialization_task_source_spool(
			std::move(source_storage), make_materialization_sha256_accumulator());
		require(source && (*source)->append(source_bytes.first(3U)) &&
					(*source)->append(source_bytes.subspan(3U)) && (*source)->seal(),
				"task.v3 source spool preparation failed");

		auto streaming_task = task;
		streaming_task.source.clear();
		streaming_task.source_content_base64.clear();
		auto task_storage = std::make_unique<controlled_spool>(1U);
		auto input = make_materialization_task_input_spool(std::move(task_storage));
		require(input.has_value(), "task-input spool construction failed");
		auto encoded = encode_task_input_streaming(streaming_task, **source, **input);
		require(encoded && encoded->source_size_bytes == source_bytes.size() &&
					encoded->task_input_bytes == expected->size() && (*input)->seal() &&
					(*input)->sealed() && (*input)->size_bytes() == expected->size() &&
					replay_all(static_cast<clang22_task_input_replay&>(**input), 4096U) ==
						*expected,
				"task.v3 streaming encoder changed canonical replay bytes");

		auto decoded_source = make_materialization_task_source_spool();
		require(decoded_source.has_value(), "decoded source spool construction failed");
		auto decoded = decode_task_input_streaming(**input, **decoded_source);
		require(decoded && decoded->input.source.empty() &&
					decoded->input.source_content_base64.empty() &&
					decoded->input.compile_unit == task.compile_unit &&
					decoded->input.selected_catalog_compile_unit ==
						task.selected_catalog_compile_unit &&
					decoded->source == source_receipt_oracle(source_bytes) &&
					decoded->task_input_bytes == expected->size() && (*decoded_source)->sealed() &&
					replay_all(**decoded_source, 5U) ==
						std::vector<std::byte>{source_bytes.begin(), source_bytes.end()},
				"task.v3 streaming decoder did not round-trip through private spools");
		auto sealed_append = (*input)->append(bytes("x"));
		auto duplicate_seal = (*input)->seal();
		require(!sealed_append && is_materialization_admission_no_response(sealed_append.error()) &&
					!duplicate_seal &&
					is_materialization_admission_no_response(duplicate_seal.error()) &&
					(*input)->sealed(),
				"sealed task-input lifecycle drift escaped the no-response boundary");
	}

	void task_input_limits_and_failures()
	{
		std::vector<std::byte> chunk(1024U * 1024U, std::byte{'q'});
		auto counting =
			std::make_unique<controlled_spool>(std::numeric_limits<std::size_t>::max(), false);
		auto bounded = make_materialization_task_input_spool(std::move(counting));
		require(bounded.has_value(), "bounded task-input spool construction failed");
		for (std::size_t index{}; index < 64U; ++index)
			require((*bounded)->append(chunk).has_value(),
					"exact 64 MiB task-input boundary was rejected");
		auto task_over_limit = (*bounded)->append(std::span{chunk}.first(1U));
		require((*bounded)->size_bytes() == maximum_clang22_task_input_bytes && !task_over_limit &&
					is_materialization_admission_no_response(task_over_limit.error()) &&
					!(*bounded)->seal() && !(*bounded)->sealed(),
				"task-input limit+1 did not poison the spool");

		{
			auto storage = std::make_unique<controlled_spool>();
			storage->throw_append_ = true;
			auto failed = make_materialization_task_input_spool(std::move(storage));
			auto result = (*failed)->append(bytes("x"));
			require(failed && !result && is_materialization_admission_no_response(result.error()),
					"task-input append bad_alloc escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_input_spool(std::move(storage));
			require(failed && (*failed)->append(bytes("x")),
					"task-input seal bad_alloc fixture append failed");
			injected->throw_seal_ = true;
			auto result = (*failed)->seal();
			require(!result && is_materialization_admission_no_response(result.error()),
					"task-input seal bad_alloc escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_input_spool(std::move(storage));
			require(failed && (*failed)->append(bytes("x")) && (*failed)->seal(),
					"task-input replay bad_alloc fixture setup failed");
			injected->throw_read_ = true;
			std::array<std::byte, 1U> output{};
			auto result = (*failed)->read_at(0U, output);
			require(!result && is_materialization_admission_no_response(result.error()),
					"task-input replay bad_alloc escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			storage->allocate_append_ = true;
			auto failed = make_materialization_task_input_spool(std::move(storage));
			auto result = (*failed)->append(bytes("x"));
			require(!result && is_materialization_admission_no_response(result.error()),
					"task-input append allocation result escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_input_spool(std::move(storage));
			require((*failed)->append(bytes("x")).has_value(),
					"task-input allocation seal fixture append failed");
			injected->allocate_seal_ = true;
			auto result = (*failed)->seal();
			require(!result && is_materialization_admission_no_response(result.error()),
					"task-input seal allocation result escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_input_spool(std::move(storage));
			require((*failed)->append(bytes("x")) && (*failed)->seal(),
					"task-input allocation replay fixture setup failed");
			injected->allocate_read_ = true;
			std::array<std::byte, 1U> output{};
			auto result = (*failed)->read_at(0U, output);
			require(!result && is_materialization_admission_no_response(result.error()),
					"task-input read allocation result escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_input_spool(std::move(storage));
			injected->size_drift_ = 1U;
			auto result = (*failed)->append(bytes("x"));
			require(!result && is_materialization_admission_no_response(result.error()),
					"task-input append size drift escaped the no-response boundary");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_input_spool(std::move(storage));
			require((*failed)->append(bytes("x")).has_value(),
					"task-input seal-state corruption fixture append failed");
			injected->report_unsealed_ = true;
			auto result = (*failed)->seal();
			require(!result && is_materialization_admission_no_response(result.error()),
					"task-input successful seal-state drift escaped the no-response boundary");
		}

		{
			auto storage = std::make_unique<controlled_spool>();
			storage->fail_append_ = true;
			auto failed = make_materialization_task_input_spool(std::move(storage));
			auto result = (*failed)->append(bytes("x"));
			require(failed && !result && result.error().code == "materialization.spool-failure" &&
						!(*failed)->seal(),
					"task-input append failure did not fail closed");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_input_spool(std::move(storage));
			require(failed && (*failed)->append(bytes("xyz")) && (*failed)->seal(),
					"task-input read-failure fixture setup failed");
			injected->zero_read_ = true;
			std::array<std::byte, 3U> output{};
			auto result = (*failed)->read_at(0U, output);
			require(!result && is_materialization_admission_no_response(result.error()) &&
						!(*failed)->sealed(),
					"premature EOF retained task-input replay authority");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			auto* injected = storage.get();
			auto failed = make_materialization_task_input_spool(std::move(storage));
			require(failed && (*failed)->append(bytes("xyz")) && (*failed)->seal(),
					"task-input overreport fixture setup failed");
			injected->overreport_read_ = true;
			std::array<std::byte, 3U> output{};
			auto result = (*failed)->read_at(0U, output);
			require(!result && is_materialization_admission_no_response(result.error()) &&
						!(*failed)->sealed(),
					"overreported read retained task-input replay authority");
		}
		{
			auto storage = std::make_unique<controlled_spool>();
			storage->fail_seal_ = true;
			auto failed = make_materialization_task_input_spool(std::move(storage));
			require(failed && (*failed)->append(bytes("x")) && !(*failed)->seal() &&
						!(*failed)->sealed(),
					"task-input seal failure exposed replay authority");
		}
		require(!make_materialization_task_input_spool(
					std::unique_ptr<materialization_replayable_spool>{}),
				"null task-input dependency was accepted");
	}
} // namespace

int main()
{
	source_receipt_and_short_replay();
	source_limits_and_failures();
	task_v3_spool_round_trip();
	task_input_limits_and_failures();
	return 0;
}
