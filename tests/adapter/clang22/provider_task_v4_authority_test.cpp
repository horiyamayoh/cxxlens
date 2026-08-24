#include "llvm/clang22/provider_task_v4_authority.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include "llvm/clang22/provider_task_v4_authority_internal.hpp"

namespace
{
	using namespace cxxlens::detail::clang22;

	[[nodiscard]] std::string semantic(const char digit)
	{
		return "semantic-v2:sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::string content(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::string process_digest(const provider_task_v4_authority_identity& value)
	{
		auto result = cxxlens::sdk::canonical_identity_digest(
			"process-channel",
			std::array{
				cxxlens::sdk::canonical_value::from_string(value.process_mode),
				cxxlens::sdk::canonical_value::from_string(value.task_id),
				cxxlens::sdk::canonical_value::from_string(value.session_id),
				cxxlens::sdk::canonical_value::from_string(value.task_v4_digest),
				cxxlens::sdk::canonical_value::from_string(value.closure_id),
				cxxlens::sdk::canonical_value::from_string(value.closure_digest),
				cxxlens::sdk::canonical_value::from_string(value.manifest_digest),
				cxxlens::sdk::canonical_value::from_string(value.transfer_digest),
				cxxlens::sdk::canonical_value::from_string(std::to_string(value.stream_id)),
				cxxlens::sdk::canonical_value::from_string(std::to_string(value.first_sequence)),
				cxxlens::sdk::canonical_value::from_integer(value.read_descriptor),
				cxxlens::sdk::canonical_value::from_integer(value.write_descriptor),
				cxxlens::sdk::canonical_value::from_string(std::to_string(value.read_device)),
				cxxlens::sdk::canonical_value::from_string(std::to_string(value.read_inode)),
				cxxlens::sdk::canonical_value::from_string(std::to_string(value.read_mode)),
				cxxlens::sdk::canonical_value::from_string(std::to_string(value.write_device)),
				cxxlens::sdk::canonical_value::from_string(std::to_string(value.write_inode)),
				cxxlens::sdk::canonical_value::from_string(std::to_string(value.write_mode)),
			});
		assert(result);
		return *result;
	}

	[[nodiscard]] provider_task_v4_authority_identity make_identity()
	{
		provider_task_v4_authority_identity value;
		value.authority_schema = std::string{provider_task_v4_authority_schema};
		value.request_schema = std::string{provider_task_v4_authority_request_schema};
		value.request_version = std::string{provider_task_v4_authority_request_version};
		value.request_digest = semantic('a');
		value.request_id = "materialization-request:" + value.request_digest;
		value.materialization_request_id = "materialization-request:one";
		value.semantic_request_digest = semantic('b');
		value.protocol_major = 2U;
		value.protocol_minor = 0U;
		value.required_features = {"task-input-chunks-v2", "task-source-closure-v2"};

		value.task_count = 1U;
		value.task_index = 0U;
		value.task_v4_digest = semantic('c');
		value.task_id = "task:" + value.task_v4_digest;
		value.provider_task_id = value.task_id;
		value.provider_execution_id = "provider-execution:one";
		value.task_schema = std::string{provider_task_v4_authority_task_schema};
		value.base_task_digest = content('0');
		value.main_logical_path = "project://src/main.cpp";
		value.logical_working_directory = "project://src";
		value.task_input_digest = content('d');
		value.normalized_invocation_digest = semantic('e');
		value.environment_digest = content('f');

		value.session_id = "provider-session:sha256:" + std::string(64U, '1');
		value.closure_digest = semantic('2');
		value.closure_id = "source-closure:" + value.closure_digest;
		value.manifest_digest = semantic('3');
		value.transfer_digest = semantic('4');

		value.process_mode = std::string{provider_task_v4_authority_process_mode};
		value.stream_id = 1U;
		value.first_sequence = 0U;
		value.read_descriptor = 4;
		value.write_descriptor = 5;
		value.read_device = 11U;
		value.read_inode = 12U;
		value.read_mode = 0100600U;
		value.write_device = 13U;
		value.write_inode = 14U;
		value.write_mode = 0100600U;
		value.process_binding_digest = process_digest(value);

		value.toolchain_digest = content('5');
		value.toolchain_family = "clang";
		value.toolchain_exact_version = "22.1.0";
		value.toolchain_target_triple = "x86_64-pc-linux-gnu";
		value.toolchain_executable = "toolchain://llvm-22/bin/clang++";
		value.toolchain_executable_digest = content('6');
		value.builtin_headers_digest = content('7');
		value.toolchain_sysroot = "sysroot://llvm-22";
		value.abi_digest = content('8');
		value.plugin_spec_digest = content('9');

		value.argument_count = 4U;
		value.longest_argument_bytes = 128U;
		value.root_count = 1U;
		value.longest_root_path_bytes = 64U;
		value.manifest_bytes = 256U;
		value.member_count = 1U;
		value.blob_count = 1U;
		value.blob_bytes = 128U;
		value.unique_blob_bytes = 128U;
		value.source_bytes = 128U;
		value.aggregate_source_bytes = 128U;
		value.output_group_count = 2U;
		value.output_bytes = 1024U;
		value.resident_bytes = 4096U;
		return value;
	}

	void positive_issue_and_determinism()
	{
		static_assert(!std::is_default_constructible_v<provider_task_v4_authority>);
		static_assert(!std::is_copy_constructible_v<provider_task_v4_authority>);
		static_assert(!std::is_copy_assignable_v<provider_task_v4_authority>);
		static_assert(std::is_move_constructible_v<provider_task_v4_authority>);
		static_assert(std::is_move_assignable_v<provider_task_v4_authority>);

		auto identity = make_identity();
		assert(validate_provider_task_v4_authority_identity(identity));
		auto expected_digest = derive_provider_task_v4_authority_digest(identity);
		assert(expected_digest);

		auto authority = issue_provider_task_v4_authority(std::move(identity));
		assert(authority);
		assert(authority->valid());
		assert(authority->authority_digest() == *expected_digest);
		auto snapshot = authority->snapshot();
		assert(snapshot);
		assert(snapshot->task_id == "task:" + snapshot->task_v4_digest);
		assert(authority->matches(*snapshot));

		auto changed = *snapshot;
		changed.stream_id = 2U;
		assert(!authority->matches(changed));
	}

	void reject_foreign_and_downgraded_identity()
	{
		{
			auto value = make_identity();
			value.session_id = "provider-session:sha256:" + std::string(64U, 'a');
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.process_binding_digest = process_digest(value);
			value.read_inode += 1U;
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.protocol_minor = 1U;
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.request_version = "2.1.0";
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.required_features = {"task-source-closure-v2", "task-input-chunks-v2"};
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.closure_id = "source-closure:" + semantic('a');
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
	}

	void reject_bounds_and_overflow()
	{
		{
			auto value = make_identity();
			value.task_count = 4097U;
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.member_count = 4097U;
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.longest_argument_bytes = 2049U;
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.longest_root_path_bytes = 4097U;
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.blob_bytes = 16U * 1024U * 1024U + 1U;
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.output_group_count = 4097U;
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			value.member_count = std::numeric_limits<std::uint64_t>::max();
			value.blob_count = std::numeric_limits<std::uint64_t>::max();
			assert(!issue_provider_task_v4_authority(std::move(value)));
		}
		{
			auto value = make_identity();
			provider_task_v4_authority_limits limits;
			limits.maximum_tasks = 4097U;
			assert(!issue_provider_task_v4_authority(std::move(value), limits));
		}
	}

	void move_after_use_and_replay_are_rejected()
	{
		auto value = make_identity();
		auto authority = issue_provider_task_v4_authority(std::move(value));
		assert(authority);
		auto moved = std::move(*authority);
		assert(!authority->valid());
		assert(authority->authority_digest().empty());
		assert(!authority->snapshot());
		assert(moved.valid());

		auto consumed = std::move(moved).consume();
		assert(consumed);
		assert(!moved.valid());
		assert(!std::move(moved).consume());
		assert(!std::move(*authority).consume());
	}
} // namespace

int main()
{
	positive_issue_and_determinism();
	reject_foreign_and_downgraded_identity();
	reject_bounds_and_overflow();
	move_after_use_and_replay_are_rejected();
}
