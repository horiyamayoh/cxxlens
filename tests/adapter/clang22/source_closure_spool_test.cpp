#include "llvm/clang22/source_closure_spool.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang22/materialization_json.hpp"

namespace
{
	using namespace cxxlens::detail::clang22;
	using cxxlens::detail::clang22::materialization::json_value;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	template <class T>
	void require_result(const cxxlens::sdk::result<T>& value, const std::string_view message)
	{
		require(value.has_value(), message);
	}

	[[nodiscard]] std::shared_ptr<const std::string> content(std::string value)
	{
		return std::make_shared<const std::string>(std::move(value));
	}

	[[nodiscard]] std::string role_name(const source_closure_role role)
	{
		switch (role)
		{
			case source_closure_role::main:
				return "main";
			case source_closure_role::header:
				return "header";
			case source_closure_role::generated:
				return "generated";
			case source_closure_role::forced_include:
				return "forced-include";
			case source_closure_role::macro_file:
				return "macro-file";
		}
		return {};
	}

	[[nodiscard]] std::string encoding_name(const source_closure_encoding encoding)
	{
		switch (encoding)
		{
			case source_closure_encoding::utf8:
				return "utf8";
			case source_closure_encoding::utf16le:
				return "utf16le";
			case source_closure_encoding::utf16be:
				return "utf16be";
			case source_closure_encoding::locale_dependent:
				return "locale_dependent";
			case source_closure_encoding::binary_or_unknown:
				return "binary_or_unknown";
		}
		return {};
	}

	[[nodiscard]] json_value manifest_for(const source_closure_snapshot& snapshot)
	{
		std::vector<json_value> members;
		for (const auto& item : snapshot.members)
		{
			json_value::object_type fields;
			fields.emplace("content_digest", json_value::string(item.content_digest).value());
			fields.emplace("encoding", json_value::string(encoding_name(item.encoding)).value());
			fields.emplace("file_id", json_value::string(item.file_id).value());
			fields.emplace("logical_path", json_value::string(item.logical_path).value());
			fields.emplace("read_only", json_value::boolean(item.read_only));
			fields.emplace("role", json_value::string(role_name(item.role)).value());
			fields.emplace("size_bytes", json_value::unsigned_integer(item.size_bytes));
			members.push_back(json_value::object(std::move(fields)).value());
		}

		std::vector<json_value> blobs;
		for (const auto& item : snapshot.blobs)
		{
			json_value::object_type fields;
			fields.emplace("content_digest", json_value::string(item.content_digest).value());
			fields.emplace("size_bytes", json_value::unsigned_integer(item.size_bytes));
			blobs.push_back(json_value::object(std::move(fields)).value());
		}

		json_value::object_type root;
		root.emplace("blobs", json_value::array(std::move(blobs)));
		root.emplace("closure_digest", json_value::string(snapshot.closure_digest).value());
		root.emplace("closure_id", json_value::string(snapshot.snapshot_id).value());
		root.emplace("members", json_value::array(std::move(members)));
		root.emplace("schema", json_value::string("cxxlens.source-closure-manifest.v1").value());
		return json_value::object(std::move(root)).value();
	}

	struct transfer_fixture
	{
		source_closure_snapshot snapshot;
		source_closure_manifest_descriptor manifest;
		std::string manifest_bytes;
		source_closure_transfer_binding binding;
	};

	[[nodiscard]] transfer_fixture fixture()
	{
		auto snapshot = make_source_closure_snapshot({
			{"project://src/main.cpp",
			 source_closure_role::main,
			 source_closure_encoding::utf8,
			 content("int main() { return 0; }\n")},
			{"project://include/value.hpp",
			 source_closure_role::header,
			 source_closure_encoding::utf8,
			 content("#pragma once\n#define VALUE 7\n")},
		});
		require_result(snapshot, "source-closure spool fixture was rejected");
		const auto manifest_value = manifest_for(*snapshot);
		const auto manifest_bytes =
			cxxlens::detail::clang22::materialization::canonical_json(manifest_value);
		auto manifest_digest =
			cxxlens::sdk::semantic_digest("cxxlens.source-closure-manifest.v1", manifest_bytes);
		require_result(manifest_digest, "manifest fixture digest failed");

		transfer_fixture output{std::move(*snapshot), {}, manifest_bytes, {}};
		output.binding.session_id = "provider-session:sha256:" + std::string(64U, '1');
		output.binding.task_v4_digest = "semantic-v2:sha256:" + std::string(64U, '2');
		output.binding.task_id = "task:" + output.binding.task_v4_digest;
		output.binding.closure_digest = output.snapshot.closure_digest;
		output.binding.closure_id = output.snapshot.snapshot_id;
		output.binding.manifest_digest = *manifest_digest;
		output.binding.first_sequence = 0U;
		output.manifest = {output.binding.session_id,
						   output.binding.task_id,
						   output.binding.task_v4_digest,
						   output.binding.closure_id,
						   output.binding.closure_digest,
						   output.binding.manifest_digest,
						   static_cast<std::uint64_t>(output.manifest_bytes.size()),
						   static_cast<std::uint64_t>(output.manifest_bytes.size()),
						   1U};
		return output;
	}

	[[nodiscard]] std::span<const std::byte> as_bytes(const std::string& value)
	{
		return std::as_bytes(std::span{value.data(), value.size()});
	}

	void exercise_positive_transfer()
	{
		auto input = fixture();
		source_closure_spool spool;
		require_result(spool.begin_manifest(input.manifest), "spool rejected manifest begin");
		require_result(spool.append_manifest(as_bytes(input.manifest_bytes)),
					   "spool rejected manifest bytes");
		auto summary = spool.finish_manifest(input.binding.manifest_digest);
		require_result(summary, "spool rejected canonical manifest");
		require(summary->member_count == input.snapshot.members.size(),
				"spool returned the wrong member census");
		require(summary->blob_count == input.snapshot.blobs.size(),
				"spool returned the wrong blob census");

		std::vector<source_closure_blob_receipt> receipts;
		for (std::size_t ordinal{}; ordinal < input.snapshot.blobs.size(); ++ordinal)
		{
			const auto& expected = input.snapshot.blobs[ordinal];
			const source_closure_blob_descriptor descriptor{input.binding.session_id,
															input.binding.task_id,
															input.binding.closure_digest,
															static_cast<std::uint64_t>(ordinal),
															expected.content_digest,
															expected.size_bytes,
															expected.size_bytes,
															1U};
			require_result(spool.begin_blob(descriptor), "spool rejected blob begin");
			require_result(spool.append_blob(as_bytes(*expected.content)),
						   "spool rejected blob bytes");
			const source_closure_blob_receipt receipt{
				static_cast<std::uint64_t>(ordinal), expected.content_digest, expected.size_bytes};
			require_result(spool.finish_blob(receipt), "spool rejected blob seal");
			receipts.push_back(receipt);
		}

		auto receipts_digest = source_closure_blob_receipts_digest(receipts);
		require_result(receipts_digest, "receipt digest failed");
		auto transfer_digest = source_closure_transfer_digest(
			input.binding, *receipts_digest, receipts.size(), summary->total_blob_bytes);
		require_result(transfer_digest, "transfer digest failed");
		auto credentials = spool.finish_closure(*transfer_digest);
		require_result(credentials, "spool rejected closure seal");
		require(credentials->transfer_digest == *transfer_digest,
				"spool credential was not bound to transfer digest");
		require(spool.sealed(), "spool did not enter sealed state");
		auto snapshot = spool.snapshot();
		require_result(snapshot, "spool did not expose a validated snapshot");
		require(snapshot->snapshot_id == input.snapshot.snapshot_id,
				"spool snapshot identity changed");
		require(snapshot->members.size() == input.snapshot.members.size(),
				"spool member census changed");
		for (std::size_t index{}; index < snapshot->members.size(); ++index)
		{
			const auto& actual = snapshot->members[index];
			const auto& expected = input.snapshot.members[index];
			require(actual.file_id == expected.file_id &&
						actual.logical_path == expected.logical_path &&
						actual.role == expected.role && actual.encoding == expected.encoding &&
						actual.size_bytes == expected.size_bytes &&
						actual.content_digest == expected.content_digest &&
						actual.read_only == expected.read_only,
					"spool member metadata changed");
		}
		require(spool.retained_bytes() == summary->total_blob_bytes,
				"spool retained bytes exceeded the sealed blob census");

		auto cleanup = spool.cleanup();
		require_result(cleanup, "spool cleanup did not issue a receipt");
		require(cleanup->starts_with("cleanup-receipt:semantic-v2:sha256:"),
				"spool cleanup receipt was not typed");
		require(!spool.snapshot(), "cleaned spool exposed a stale snapshot");
		auto repeated_cleanup = spool.cleanup();
		require_result(repeated_cleanup, "repeated cleanup lost its receipt");
		require(*repeated_cleanup == *cleanup, "cleanup receipt was not stable");
	}

	void exercise_negative_boundaries()
	{
		auto input = fixture();
		source_closure_spool noncanonical;
		auto descriptor = input.manifest;
		++descriptor.total_bytes;
		require_result(noncanonical.begin_manifest(descriptor), "noncanonical setup failed");
		std::string noncanonical_bytes = input.manifest_bytes + " ";
		require_result(noncanonical.append_manifest(as_bytes(noncanonical_bytes)),
					   "noncanonical bytes setup failed");
		require(!noncanonical.finish_manifest(input.binding.manifest_digest),
				"noncanonical manifest was accepted");

		source_closure_spool tampered;
		require_result(tampered.begin_manifest(input.manifest), "tampered setup failed");
		require_result(tampered.append_manifest(as_bytes(input.manifest_bytes)),
					   "tampered manifest setup failed");
		require_result(tampered.finish_manifest(input.binding.manifest_digest),
					   "tampered manifest was rejected");
		const auto& expected = input.snapshot.blobs.front();
		const source_closure_blob_descriptor blob{input.binding.session_id,
												  input.binding.task_id,
												  input.binding.closure_digest,
												  0U,
												  expected.content_digest,
												  expected.size_bytes,
												  expected.size_bytes,
												  1U};
		require_result(tampered.begin_blob(blob), "tampered blob setup failed");
		std::string wrong = *expected.content;
		wrong.front() = wrong.front() == 'x' ? 'y' : 'x';
		require_result(tampered.append_blob(as_bytes(wrong)), "tampered blob bytes setup failed");
		require(!tampered.finish_blob({0U, expected.content_digest, expected.size_bytes}),
				"tampered blob content was accepted");

		auto limits = source_closure_transport_limits{};
		limits.maximum_manifest_bytes = input.manifest_bytes.size() - 1U;
		source_closure_spool bounded{limits};
		require(!bounded.begin_manifest(input.manifest), "manifest bound was not enforced");
	}
} // namespace

int main()
{
	exercise_positive_transfer();
	exercise_negative_boundaries();
	return 0;
}
