#include "source_closure_spool.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "materialization_json.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using materialization::json_value;

		constexpr std::string_view semantic_prefix{"semantic-v2:sha256:"};
		constexpr std::string_view content_prefix{"sha256:"};
		constexpr std::size_t invalid_blob_ordinal = std::numeric_limits<std::size_t>::max();

		[[nodiscard]] sdk::error spool_io_failure(std::string field, std::string detail = {})
		{
			return {"source-closure.spool-io", std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string errno_detail(const int value)
		{
			return std::to_string(value);
		}

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<void> manifest_failure(std::string field, std::string detail = {})
		{
			return sdk::unexpected(
				failure("source-closure.manifest-invalid", std::move(field), std::move(detail)));
		}

		[[nodiscard]] std::span<const std::byte> bytes(const std::string& value) noexcept
		{
			return std::as_bytes(std::span{value.data(), value.size()});
		}

		[[nodiscard]] bool lower_hex(const std::string_view value) noexcept
		{
			return std::ranges::all_of(value,
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] bool typed_digest(const std::string_view value,
										const std::string_view prefix) noexcept
		{
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				lower_hex(value.substr(prefix.size()));
		}

		[[nodiscard]] sdk::result<const json_value*>
		member(const json_value& object, const std::string_view name, const std::string_view field)
		{
			if (object.as_object() == nullptr)
				return sdk::unexpected(
					failure("source-closure.manifest-invalid", std::string{field}, "object"));
			const auto* value = object.member(name);
			if (value == nullptr)
				return sdk::unexpected(failure("source-closure.manifest-invalid",
											   std::string{field},
											   "missing:" + std::string{name}));
			return value;
		}

		[[nodiscard]] sdk::result<std::string> string_member(const json_value& object,
															 const std::string_view name,
															 const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (const auto* string = (*value)->as_string())
				return *string;
			return sdk::unexpected(
				failure("source-closure.manifest-invalid", std::string{field}, "string"));
		}

		[[nodiscard]] sdk::result<std::uint64_t> unsigned_member(const json_value& object,
																 const std::string_view name,
																 const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (const auto* unsigned_value = (*value)->as_unsigned_integer())
				return *unsigned_value;
			if (const auto* signed_value = (*value)->as_signed_integer();
				signed_value != nullptr && *signed_value >= 0)
				return static_cast<std::uint64_t>(*signed_value);
			return sdk::unexpected(
				failure("source-closure.manifest-invalid", std::string{field}, "unsigned-integer"));
		}

		[[nodiscard]] sdk::result<bool> boolean_member(const json_value& object,
													   const std::string_view name,
													   const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (const auto* boolean = (*value)->as_boolean())
				return *boolean;
			return sdk::unexpected(
				failure("source-closure.manifest-invalid", std::string{field}, "boolean"));
		}

		[[nodiscard]] sdk::result<const json_value*> array_member(const json_value& object,
																  const std::string_view name,
																  const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if ((*value)->as_array() == nullptr)
				return sdk::unexpected(
					failure("source-closure.manifest-invalid", std::string{field}, "array"));
			return value;
		}

		[[nodiscard]] bool valid_content_digest(const std::string_view value) noexcept
		{
			return typed_digest(value, content_prefix);
		}

		[[nodiscard]] sdk::result<source_closure_role> parse_role(const std::string_view value)
		{
			if (value == "main")
				return source_closure_role::main;
			if (value == "header")
				return source_closure_role::header;
			if (value == "generated")
				return source_closure_role::generated;
			if (value == "forced-include")
				return source_closure_role::forced_include;
			if (value == "macro-file")
				return source_closure_role::macro_file;
			return sdk::unexpected(
				failure("source-closure.manifest-invalid", "member.role", std::string{value}));
		}

		[[nodiscard]] sdk::result<source_closure_encoding>
		parse_encoding(const std::string_view value)
		{
			if (value == "utf8")
				return source_closure_encoding::utf8;
			if (value == "utf16le")
				return source_closure_encoding::utf16le;
			if (value == "utf16be")
				return source_closure_encoding::utf16be;
			if (value == "locale_dependent")
				return source_closure_encoding::locale_dependent;
			if (value == "binary_or_unknown")
				return source_closure_encoding::binary_or_unknown;
			return sdk::unexpected(
				failure("source-closure.manifest-invalid", "member.encoding", std::string{value}));
		}

		[[nodiscard]] bool valid_semantic_id(const std::string_view value,
											 const std::string_view prefix) noexcept
		{
			return typed_digest(value, prefix);
		}

		[[nodiscard]] sdk::result<void> exact_fields(const json_value& value,
													 const std::span<const std::string_view> fields,
													 const std::string_view field)
		{
			if (value.as_object() == nullptr || !value.has_exact_members(fields))
				return manifest_failure(std::string{field}, "field-census");
			return {};
		}

		[[nodiscard]] sdk::result<std::string>
		credential_digest(const std::string_view prefix,
						  const std::string_view transfer_digest,
						  const source_closure_manifest_descriptor& descriptor)
		{
			std::string projection;
			projection.reserve(prefix.size() + transfer_digest.size() +
							   descriptor.session_id.size() + descriptor.task_id.size() +
							   descriptor.closure_digest.size() +
							   descriptor.manifest_digest.size() + 8U);
			projection.append("source-closure-spool.v1\n");
			projection.append(prefix);
			projection.push_back('\n');
			projection.append(descriptor.session_id);
			projection.push_back('\n');
			projection.append(descriptor.task_id);
			projection.push_back('\n');
			projection.append(descriptor.closure_digest);
			projection.push_back('\n');
			projection.append(descriptor.manifest_digest);
			projection.push_back('\n');
			projection.append(transfer_digest);
			auto digest = sdk::semantic_digest("cxxlens.source-closure-spool.v1", projection);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			return std::string{prefix} + *digest;
		}

		[[nodiscard]] sdk::result<void>
		validate_descriptor_ids(const source_closure_manifest_descriptor& descriptor)
		{
			if (!typed_digest(descriptor.manifest_digest, semantic_prefix) ||
				!typed_digest(descriptor.closure_digest, semantic_prefix) ||
				!typed_digest(descriptor.task_v4_digest, semantic_prefix) ||
				!typed_digest(descriptor.session_id, "provider-session:sha256:") ||
				!typed_digest(descriptor.task_id, "task:semantic-v2:sha256:") ||
				descriptor.task_id != "task:" + descriptor.task_v4_digest ||
				!typed_digest(descriptor.closure_id, "source-closure:semantic-v2:sha256:") ||
				descriptor.closure_id != "source-closure:" + descriptor.closure_digest)
				return sdk::unexpected(
					failure("source-closure.task-binding-mismatch", "manifest-descriptor"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_manifest_blob_references(const source_closure_snapshot& snapshot)
		{
			std::vector<bool> referenced(snapshot.blobs.size(), false);
			for (const auto& member : snapshot.members)
			{
				const auto found = std::ranges::lower_bound(snapshot.blobs,
															member.content_digest,
															{},
															&source_closure_blob::content_digest);
				if (found == snapshot.blobs.end() || found->content_digest != member.content_digest)
					return manifest_failure("member.content_digest", "blob-missing");
				if (found->size_bytes != member.size_bytes)
					return manifest_failure("member.size_bytes", "blob-size-mismatch");
				referenced[static_cast<std::size_t>(found - snapshot.blobs.begin())] = true;
			}
			if (std::ranges::find(referenced, false) != referenced.end())
				return manifest_failure("blobs", "orphan-blob");
			return {};
		}

		[[nodiscard]] sdk::result<int> create_private_memfd()
		{
#if defined(__linux__)
			const auto descriptor = static_cast<int>(::syscall(
				SYS_memfd_create, "cxxlens-source-closure", MFD_CLOEXEC | MFD_ALLOW_SEALING));
			if (descriptor < 0)
				return sdk::unexpected(
					spool_io_failure("private-spool", "memfd-create-" + errno_detail(errno)));
			return descriptor;
#else
			return sdk::unexpected(
				spool_io_failure("private-spool", "sealed-private-backend-unavailable"));
#endif
		}

		[[nodiscard]] bool
		add_u64(const std::uint64_t left, const std::uint64_t right, std::uint64_t& output) noexcept
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return false;
			output = left + right;
			return true;
		}
	} // namespace

	source_closure_spool::source_closure_spool(const source_closure_transport_limits limits)
		: limits_{limits}, current_blob_ordinal_{invalid_blob_ordinal}
	{
	}

	source_closure_spool::~source_closure_spool()
	{
		close_private_spool();
	}

	void source_closure_spool::close_private_spool() noexcept
	{
#if defined(__linux__)
		if (private_spool_fd_ >= 0)
			(void)::close(private_spool_fd_);
#endif
		private_spool_fd_ = -1;
	}

	sdk::result<void> source_closure_spool::ensure_private_spool()
	{
		if (private_spool_fd_ >= 0)
			return {};
		auto descriptor = create_private_memfd();
		if (!descriptor)
			return sdk::unexpected(std::move(descriptor.error()));
		private_spool_fd_ = *descriptor;
		return {};
	}

	sdk::result<void>
	source_closure_spool::append_private_spool(const std::span<const std::byte> payload)
	{
		if (payload.empty())
			return {};
#if defined(__linux__)
		if (private_spool_fd_ < 0)
			return sdk::unexpected(spool_io_failure("private-spool", "descriptor-unset"));
		if (payload.size() > std::numeric_limits<std::uint64_t>::max() - private_spool_bytes_)
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "private-spool", "byte-overflow"));
		std::size_t offset{};
		while (offset < payload.size())
		{
			const auto absolute = private_spool_bytes_ + offset;
			if (absolute > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "private-spool", "offset"));
			const auto count = ::pwrite(
				private_spool_fd_,
				payload.data() + offset,
				std::min<std::size_t>(payload.size() - offset, std::numeric_limits<ssize_t>::max()),
				static_cast<off_t>(absolute));
			if (count > 0)
			{
				offset += static_cast<std::size_t>(count);
				continue;
			}
			if (count < 0 && errno == EINTR)
				continue;
			return sdk::unexpected(spool_io_failure(
				"private-spool", count == 0 ? "zero-progress" : "write-" + errno_detail(errno)));
		}
		private_spool_bytes_ += payload.size();
		return {};
#else
		(void)payload;
		return sdk::unexpected(
			spool_io_failure("private-spool", "sealed-private-backend-unavailable"));
#endif
	}

	sdk::result<std::string> source_closure_spool::read_private_blob(const std::uint64_t offset,
																	 const std::uint64_t size) const
	{
		if (size > std::numeric_limits<std::size_t>::max() ||
			offset > std::numeric_limits<std::uint64_t>::max() - size)
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "private-spool", "range"));
		std::string output;
		try
		{
			output.resize(static_cast<std::size_t>(size));
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("source-closure.limit-exceeded", "blob", "allocation"));
		}
#if defined(__linux__)
		if (private_spool_fd_ < 0)
			return sdk::unexpected(spool_io_failure("private-spool", "descriptor-unset"));
		std::size_t received{};
		while (received < output.size())
		{
			const auto absolute = offset + received;
			if (absolute > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "private-spool", "offset"));
			const auto count = ::pread(private_spool_fd_,
									   output.data() + received,
									   std::min<std::size_t>(output.size() - received,
															 std::numeric_limits<ssize_t>::max()),
									   static_cast<off_t>(absolute));
			if (count > 0)
			{
				received += static_cast<std::size_t>(count);
				continue;
			}
			if (count < 0 && errno == EINTR)
				continue;
			return sdk::unexpected(spool_io_failure(
				"private-spool", count == 0 ? "unexpected-eof" : "read-" + errno_detail(errno)));
		}
#else
		(void)offset;
		(void)size;
		return sdk::unexpected(
			spool_io_failure("private-spool", "sealed-private-backend-unavailable"));
#endif
		return output;
	}

	sdk::result<void> source_closure_spool::seal_private_spool() const
	{
#if defined(__linux__)
		if (private_spool_fd_ < 0)
			return sdk::unexpected(spool_io_failure("private-spool", "descriptor-unset"));
		constexpr int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
		if (::fcntl(private_spool_fd_, F_ADD_SEALS, required) != 0)
			return sdk::unexpected(
				spool_io_failure("private-spool", "seal-" + errno_detail(errno)));
		const auto observed = ::fcntl(private_spool_fd_, F_GET_SEALS);
		if (observed < 0 || (observed & required) != required)
			return sdk::unexpected(spool_io_failure("private-spool", "seal-observation"));
		return {};
#else
		return sdk::unexpected(
			spool_io_failure("private-spool", "sealed-private-backend-unavailable"));
#endif
	}

	sdk::result<void> source_closure_spool::validate_sealed_metadata() const
	{
		if (snapshot_.members.empty() || snapshot_.blobs.empty() ||
			snapshot_.members.size() > limits_.maximum_members ||
			snapshot_.blobs.size() > limits_.maximum_unique_blobs ||
			blob_offsets_.size() != snapshot_.blobs.size())
			return sdk::unexpected(failure("source-closure.limit-exceeded", "closure.census"));
		std::uint64_t total{};
		for (const auto& blob : snapshot_.blobs)
		{
			std::uint64_t next_total{};
			if (!valid_content_digest(blob.content_digest) ||
				blob.size_bytes > limits_.maximum_blob_bytes ||
				!add_u64(total, blob.size_bytes, next_total) ||
				next_total > limits_.maximum_unique_blob_bytes)
				return sdk::unexpected(failure("source-closure.digest-mismatch", "closure.blob"));
			total = next_total;
		}
		if (total != completed_blob_bytes_ || total != private_spool_bytes_)
			return sdk::unexpected(failure("source-closure.digest-mismatch", "closure.bytes"));
		return validate_manifest_blob_references(snapshot_);
	}

	sdk::result<void> source_closure_spool::reject(std::string field, std::string detail) const
	{
		return sdk::unexpected(
			failure("source-closure.protocol-state-invalid", std::move(field), std::move(detail)));
	}

	sdk::result<void> source_closure_spool::reject_limit(std::string field) const
	{
		return sdk::unexpected(
			failure("source-closure.limit-exceeded", std::move(field), "task-local-spool"));
	}

	sdk::result<void>
	source_closure_spool::begin_manifest(const source_closure_manifest_descriptor& descriptor)
	{
		if (state_ != state::fresh)
			return reject("manifest", "already-open");
		auto valid_ids = validate_descriptor_ids(descriptor);
		if (!valid_ids)
			return sdk::unexpected(std::move(valid_ids.error()));
		if (descriptor.total_bytes == 0U ||
			descriptor.total_bytes > limits_.maximum_manifest_bytes ||
			descriptor.total_bytes > limits_.maximum_task_spool_bytes ||
			descriptor.chunk_bytes == 0U ||
			descriptor.chunk_bytes > limits_.maximum_chunk_payload_bytes ||
			descriptor.total_bytes > std::numeric_limits<std::size_t>::max())
			return reject_limit("manifest");
		if (auto private_spool = ensure_private_spool(); !private_spool)
			return sdk::unexpected(std::move(private_spool.error()));
		try
		{
			manifest_descriptor_ = descriptor;
			manifest_bytes_.reserve(static_cast<std::size_t>(descriptor.total_bytes));
			blob_offsets_.reserve(static_cast<std::size_t>(limits_.maximum_unique_blobs));
		}
		catch (const std::bad_alloc&)
		{
			return reject_limit("manifest");
		}
		manifest_received_bytes_ = 0U;
		state_ = state::manifest_open;
		return {};
	}

	sdk::result<void>
	source_closure_spool::append_manifest(const std::span<const std::byte> payload)
	{
		if (state_ != state::manifest_open)
			return reject("manifest-chunk", "not-open");
		if (payload.empty() || payload.size() > limits_.maximum_chunk_payload_bytes ||
			manifest_received_bytes_ > manifest_descriptor_.total_bytes ||
			payload.size() > manifest_descriptor_.total_bytes - manifest_received_bytes_)
			return reject("manifest-chunk", "byte-count");
		try
		{
			manifest_bytes_.append(reinterpret_cast<const char*>(payload.data()), payload.size());
		}
		catch (const std::bad_alloc&)
		{
			return reject_limit("manifest");
		}
		manifest_received_bytes_ += payload.size();
		return {};
	}

	sdk::result<source_closure_manifest_summary>
	source_closure_spool::finish_manifest(const std::string_view manifest_digest)
	{
		if (state_ != state::manifest_open)
			return sdk::unexpected(
				failure("source-closure.protocol-state-invalid", "manifest", "not-open"));
		if (manifest_received_bytes_ != manifest_descriptor_.total_bytes ||
			manifest_digest != manifest_descriptor_.manifest_digest)
			return sdk::unexpected(
				failure("source-closure.manifest-invalid", "manifest", "census"));

		try
		{
			materialization::json_limits json_limits;
			json_limits.max_input_bytes = static_cast<std::size_t>(limits_.maximum_manifest_bytes);
			json_limits.max_depth = 8U;
			json_limits.max_array_elements = static_cast<std::size_t>(limits_.maximum_members);
			json_limits.max_object_members = 16U;
			json_limits.max_string_bytes =
				static_cast<std::size_t>(limits_.maximum_logical_path_bytes);
			json_limits.max_total_string_bytes =
				static_cast<std::size_t>(limits_.maximum_manifest_bytes);
			json_limits.max_total_values =
				static_cast<std::size_t>(limits_.maximum_members * 16U + 32U);
			auto parsed =
				materialization::parse_json_object(std::move(manifest_bytes_), json_limits);
			if (!parsed)
				return sdk::unexpected(
					failure("source-closure.manifest-invalid", "manifest", parsed.error().detail));
			if (materialization::canonical_json(parsed->root()) != parsed->raw_bytes())
				return sdk::unexpected(
					failure("source-closure.manifest-invalid", "manifest", "noncanonical"));
			auto observed_digest =
				sdk::semantic_digest(source_closure_manifest_digest_domain, parsed->raw_bytes());
			if (!observed_digest || *observed_digest != manifest_digest)
				return sdk::unexpected(
					failure("source-closure.digest-mismatch", "manifest_digest"));

			constexpr std::array<std::string_view, 5U> root_fields{
				"blobs", "closure_digest", "closure_id", "members", "schema"};
			if (auto valid = exact_fields(parsed->root(), root_fields, "manifest"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto schema = string_member(parsed->root(), "schema", "manifest.schema");
			auto closure_id = string_member(parsed->root(), "closure_id", "manifest.closure_id");
			auto closure_digest =
				string_member(parsed->root(), "closure_digest", "manifest.closure_digest");
			if (!schema || !closure_id || !closure_digest ||
				*schema != source_closure_manifest_schema ||
				*closure_id != manifest_descriptor_.closure_id ||
				*closure_digest != manifest_descriptor_.closure_digest ||
				!valid_semantic_id(*closure_digest, semantic_prefix) ||
				*closure_id != "source-closure:" + *closure_digest)
				return sdk::unexpected(
					failure("source-closure.task-binding-mismatch", "manifest.closure"));
			auto members_value = array_member(parsed->root(), "members", "manifest.members");
			auto blobs_value = array_member(parsed->root(), "blobs", "manifest.blobs");
			if (!members_value || !blobs_value)
				return sdk::unexpected(
					failure("source-closure.manifest-invalid", "manifest.census"));
			const auto& member_values = *(*members_value)->as_array();
			const auto& blob_values = *(*blobs_value)->as_array();
			if (member_values.empty() || member_values.size() > limits_.maximum_members ||
				blob_values.empty() || blob_values.size() > limits_.maximum_unique_blobs)
				return sdk::unexpected(failure("source-closure.limit-exceeded", "manifest.census"));

			std::vector<source_closure_member> members;
			members.reserve(member_values.size());
			std::string previous_path;
			for (const auto& value : member_values)
			{
				constexpr std::array<std::string_view, 7U> fields{"content_digest",
																  "encoding",
																  "file_id",
																  "logical_path",
																  "read_only",
																  "role",
																  "size_bytes"};
				if (auto valid = exact_fields(value, fields, "manifest.member"); !valid)
					return sdk::unexpected(std::move(valid.error()));
				auto content_digest =
					string_member(value, "content_digest", "member.content_digest");
				auto encoding = string_member(value, "encoding", "member.encoding");
				auto file_id = string_member(value, "file_id", "member.file_id");
				auto logical_path = string_member(value, "logical_path", "member.logical_path");
				auto read_only = boolean_member(value, "read_only", "member.read_only");
				auto role = string_member(value, "role", "member.role");
				auto size = unsigned_member(value, "size_bytes", "member.size_bytes");
				if (!content_digest || !encoding || !file_id || !logical_path || !read_only ||
					!role || !size)
					return sdk::unexpected(
						failure("source-closure.manifest-invalid", "manifest.member", "field"));
				auto parsed_role = parse_role(*role);
				auto parsed_encoding = parse_encoding(*encoding);
				if (!parsed_role || !parsed_encoding || !*read_only ||
					!valid_content_digest(*content_digest) || *size > limits_.maximum_blob_bytes)
					return sdk::unexpected(
						failure("source-closure.manifest-invalid", "manifest.member", "value"));
				source_closure_member member_value{*file_id,
												   *logical_path,
												   *parsed_role,
												   *parsed_encoding,
												   *size,
												   *content_digest,
												   true};
				if (auto valid = member_value.validate(); !valid)
					return sdk::unexpected(failure("source-closure.manifest-invalid",
												   "manifest.member",
												   valid.error().detail));
				if (!previous_path.empty() && previous_path >= member_value.logical_path)
					return sdk::unexpected(
						failure("source-closure.manifest-invalid", "manifest.member.order"));
				previous_path = member_value.logical_path;
				members.push_back(std::move(member_value));
			}

			std::vector<source_closure_blob> blobs;
			blobs.reserve(blob_values.size());
			std::string previous_digest;
			std::uint64_t total_blob_bytes{};
			for (const auto& value : blob_values)
			{
				constexpr std::array<std::string_view, 2U> fields{"content_digest", "size_bytes"};
				if (auto valid = exact_fields(value, fields, "manifest.blob"); !valid)
					return sdk::unexpected(std::move(valid.error()));
				auto content_digest = string_member(value, "content_digest", "blob.content_digest");
				auto size = unsigned_member(value, "size_bytes", "blob.size_bytes");
				if (!content_digest || !size || !valid_content_digest(*content_digest) ||
					*size > limits_.maximum_blob_bytes ||
					(total_blob_bytes > limits_.maximum_unique_blob_bytes - *size) ||
					(!previous_digest.empty() && previous_digest >= *content_digest))
					return sdk::unexpected(
						failure("source-closure.manifest-invalid", "manifest.blob", "value"));
				previous_digest = *content_digest;
				total_blob_bytes += *size;
				blobs.push_back({*content_digest, *size, {}});
			}

			source_closure_snapshot snapshot{manifest_descriptor_.closure_id,
											 *closure_digest,
											 std::move(members),
											 std::move(blobs)};
			if (auto valid = validate_manifest_blob_references(snapshot); !valid)
				return sdk::unexpected(std::move(valid.error()));
			snapshot_ = std::move(snapshot);
			state_ = state::manifest_validated;
			std::string{}.swap(manifest_bytes_);
			return source_closure_manifest_summary{
				manifest_descriptor_.closure_id,
				manifest_descriptor_.closure_digest,
				manifest_descriptor_.manifest_digest,
				static_cast<std::uint64_t>(snapshot_.members.size()),
				static_cast<std::uint64_t>(snapshot_.blobs.size()),
				total_blob_bytes};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "manifest", "task-local-spool"));
		}
	}

	sdk::result<void>
	source_closure_spool::begin_blob(const source_closure_blob_descriptor& descriptor)
	{
		if (state_ != state::manifest_validated && state_ != state::blob_sealed)
			return reject("blob", "not-ready");
		if (descriptor.session_id != manifest_descriptor_.session_id ||
			descriptor.task_id != manifest_descriptor_.task_id ||
			descriptor.closure_digest != manifest_descriptor_.closure_digest ||
			descriptor.blob_ordinal != completed_blob_count_ ||
			descriptor.blob_ordinal >= snapshot_.blobs.size())
			return sdk::unexpected(
				failure("source-closure.task-binding-mismatch", "blob-descriptor"));
		const auto& expected = snapshot_.blobs[static_cast<std::size_t>(descriptor.blob_ordinal)];
		if (descriptor.blob_digest != expected.content_digest ||
			descriptor.total_bytes != expected.size_bytes ||
			descriptor.total_bytes > limits_.maximum_blob_bytes)
			return sdk::unexpected(failure("source-closure.digest-mismatch", "blob-descriptor"));
		if (manifest_descriptor_.total_bytes > limits_.maximum_task_spool_bytes ||
			completed_blob_bytes_ >
				limits_.maximum_task_spool_bytes - manifest_descriptor_.total_bytes ||
			descriptor.total_bytes > limits_.maximum_task_spool_bytes -
					manifest_descriptor_.total_bytes - completed_blob_bytes_)
			return reject_limit("blob");
		if (descriptor.total_bytes > std::numeric_limits<std::size_t>::max())
			return reject_limit("blob");
		if (auto private_spool = ensure_private_spool(); !private_spool)
			return sdk::unexpected(std::move(private_spool.error()));
		current_blob_ordinal_ = static_cast<std::size_t>(descriptor.blob_ordinal);
		current_blob_digest_ = descriptor.blob_digest;
		current_blob_size_ = descriptor.total_bytes;
		current_blob_offset_ = private_spool_bytes_;
		current_blob_received_bytes_ = 0U;
		state_ = state::blob_open;
		return {};
	}

	sdk::result<void> source_closure_spool::append_blob(const std::span<const std::byte> payload)
	{
		if (state_ != state::blob_open)
			return reject("blob-chunk", "not-open");
		if (payload.empty() || payload.size() > limits_.maximum_chunk_payload_bytes ||
			current_blob_received_bytes_ > current_blob_size_ ||
			payload.size() > current_blob_size_ - current_blob_received_bytes_)
			return reject("blob-chunk", "byte-count");
		if (auto appended = append_private_spool(payload); !appended)
			return sdk::unexpected(std::move(appended.error()));
		current_blob_received_bytes_ += payload.size();
		return {};
	}

	sdk::result<void> source_closure_spool::finish_blob(const source_closure_blob_receipt& receipt)
	{
		if (state_ != state::blob_open)
			return reject("blob", "not-open");
		if (current_blob_ordinal_ == invalid_blob_ordinal ||
			receipt.blob_ordinal != current_blob_ordinal_ ||
			receipt.blob_digest != current_blob_digest_ ||
			receipt.size_bytes != current_blob_size_ ||
			current_blob_received_bytes_ != current_blob_size_)
			return sdk::unexpected(failure("source-closure.digest-mismatch", "blob-receipt"));
		auto observed = read_private_blob(current_blob_offset_, current_blob_size_);
		if (!observed)
			return sdk::unexpected(std::move(observed.error()));
		if (sdk::content_digest(bytes(*observed)) != current_blob_digest_)
			return sdk::unexpected(failure("source-closure.digest-mismatch", "blob-content"));
		try
		{
			blob_offsets_.push_back(current_blob_offset_);
		}
		catch (const std::bad_alloc&)
		{
			return reject_limit("blob-offsets");
		}
		completed_blob_bytes_ += current_blob_size_;
		++completed_blob_count_;
		current_blob_ordinal_ = invalid_blob_ordinal;
		current_blob_digest_.clear();
		current_blob_size_ = 0U;
		current_blob_offset_ = 0U;
		current_blob_received_bytes_ = 0U;
		state_ = state::blob_sealed;
		return {};
	}

	sdk::result<std::string>
	source_closure_spool::credential(const std::string_view prefix,
									 const std::string_view transfer_digest) const
	{
		return credential_digest(prefix, transfer_digest, manifest_descriptor_);
	}

	sdk::result<source_closure_ack_credentials>
	source_closure_spool::finish_closure(const std::string_view transfer_digest)
	{
		if (state_ != state::blob_sealed || completed_blob_count_ != snapshot_.blobs.size())
			return sdk::unexpected(
				failure("source-closure.protocol-state-invalid", "closure", "not-complete"));
		if (!typed_digest(transfer_digest, semantic_prefix) ||
			completed_blob_bytes_ > limits_.maximum_unique_blob_bytes)
			return sdk::unexpected(failure("source-closure.digest-mismatch", "transfer"));
		try
		{
			if (auto valid = validate_sealed_metadata(); !valid)
				return sdk::unexpected(std::move(valid.error()));
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "closure", "task-local-spool"));
		}
		if (auto sealed = seal_private_spool(); !sealed)
			return sdk::unexpected(std::move(sealed.error()));
		try
		{
			auto spool_receipt = credential("spool-receipt:", transfer_digest);
			auto cleanup_owner = credential("cleanup-owner:", transfer_digest);
			if (!spool_receipt || !cleanup_owner)
				return sdk::unexpected(failure("source-closure.spool-io", "credentials"));
			credentials_ = {*spool_receipt, *cleanup_owner, std::string{transfer_digest}};
			transfer_digest_ = transfer_digest;
			state_ = state::closure_sealed;
			return std::move(credentials_);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("source-closure.limit-exceeded", "credentials"));
		}
	}

	sdk::result<std::string> source_closure_spool::cleanup()
	{
		if (!cleanup_receipt_.empty())
			return cleanup_receipt_;
		try
		{
			auto receipt = credential("cleanup-receipt:", transfer_digest_);
			if (!receipt)
				return sdk::unexpected(std::move(receipt.error()));
			cleanup_receipt_ = *receipt;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("source-closure.cleanup-failed", "cleanup"));
		}
		std::string{}.swap(manifest_bytes_);
		close_private_spool();
		snapshot_ = {};
		manifest_descriptor_ = {};
		manifest_received_bytes_ = 0U;
		completed_blob_count_ = 0U;
		completed_blob_bytes_ = 0U;
		current_blob_ordinal_ = invalid_blob_ordinal;
		current_blob_digest_.clear();
		current_blob_size_ = 0U;
		current_blob_offset_ = 0U;
		current_blob_received_bytes_ = 0U;
		private_spool_bytes_ = 0U;
		blob_offsets_.clear();
		state_ = state::cleaned;
		return cleanup_receipt_;
	}

	sdk::result<source_closure_snapshot> source_closure_spool::snapshot() const
	{
		if (state_ != state::closure_sealed)
			return sdk::unexpected(
				failure("source-closure.protocol-state-invalid", "snapshot", "not-sealed"));
		if (blob_offsets_.size() != snapshot_.blobs.size())
			return sdk::unexpected(failure("source-closure.spool-io", "snapshot", "blob-offsets"));
		source_closure_snapshot output = snapshot_;
		try
		{
			for (std::size_t index{}; index < output.blobs.size(); ++index)
			{
				const auto& blob = output.blobs[index];
				auto content = read_private_blob(blob_offsets_[index], blob.size_bytes);
				if (!content)
					return sdk::unexpected(std::move(content.error()));
				if (sdk::content_digest(bytes(*content)) != blob.content_digest)
					return sdk::unexpected(
						failure("source-closure.digest-mismatch", "snapshot.blob-content"));
				output.blobs[index].content =
					std::make_shared<const std::string>(std::move(*content));
			}
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "snapshot", "task-local-spool"));
		}
		return output;
	}

	std::uint64_t source_closure_spool::retained_bytes() const noexcept
	{
		const auto manifest = static_cast<std::uint64_t>(manifest_bytes_.size());
		if (private_spool_bytes_ > std::numeric_limits<std::uint64_t>::max() - manifest)
			return std::numeric_limits<std::uint64_t>::max();
		return manifest + private_spool_bytes_;
	}

	bool source_closure_spool::sealed() const noexcept
	{
		return state_ == state::closure_sealed;
	}

	source_closure_spool_relay::source_closure_spool_relay(
		const source_closure_transport_limits limits)
		: spool_{limits}
	{
	}

	// NOLINTBEGIN(bugprone-exception-escape): cleanup accesses only checked result alternatives.
	source_closure_spool_relay::~source_closure_spool_relay()
	{
		// NOLINTEND(bugprone-exception-escape)
		if (terminal_ != source_closure_relay_terminal::cleaned &&
			(spool_.sealed() || spool_.retained_bytes() != 0U))
			(void)cleanup();
	}

	sdk::result<void> source_closure_spool_relay::mark_sealed() noexcept
	{
		if (terminal_ != source_closure_relay_terminal::open || !spool_.sealed())
			return sdk::unexpected(
				failure("source-closure.protocol-state-invalid", "relay", "seal"));
		terminal_ = source_closure_relay_terminal::sealed;
		return {};
	}

	sdk::result<std::string> source_closure_spool_relay::cleanup()
	{
		if (terminal_ == source_closure_relay_terminal::cleaned ||
			terminal_ == source_closure_relay_terminal::cancelled ||
			terminal_ == source_closure_relay_terminal::connection_lost ||
			terminal_ == source_closure_relay_terminal::worker_crashed)
			return spool_.cleanup();
		auto receipt = spool_.cleanup();
		if (!receipt)
			return sdk::unexpected(std::move(receipt.error()));
		terminal_ = source_closure_relay_terminal::cleaned;
		return receipt;
	}

	sdk::result<std::string> source_closure_spool_relay::cancel()
	{
		if (terminal_ == source_closure_relay_terminal::cleaned)
			return spool_.cleanup();
		if (terminal_ == source_closure_relay_terminal::connection_lost ||
			terminal_ == source_closure_relay_terminal::worker_crashed)
			return spool_.cleanup();
		cancel_observed_ = true;
		auto receipt = spool_.cleanup();
		if (!receipt)
			return sdk::unexpected(std::move(receipt.error()));
		terminal_ = source_closure_relay_terminal::cancelled;
		return receipt;
	}

	sdk::result<std::string>
	source_closure_spool_relay::terminate(const source_closure_relay_terminal terminal,
										  const bool cancel_observed)
	{
		if (terminal != source_closure_relay_terminal::connection_lost &&
			terminal != source_closure_relay_terminal::worker_crashed)
			return sdk::unexpected(
				failure("source-closure.protocol-state-invalid", "relay", "terminal"));
		if (terminal_ == source_closure_relay_terminal::cleaned)
			return spool_.cleanup();
		if (terminal_ == source_closure_relay_terminal::connection_lost ||
			terminal_ == source_closure_relay_terminal::worker_crashed)
			return spool_.cleanup();
		cancel_observed_ = cancel_observed;
		auto receipt = spool_.cleanup();
		if (!receipt)
			return sdk::unexpected(std::move(receipt.error()));
		terminal_ = terminal;
		return receipt;
	}

	sdk::result<std::string> source_closure_spool_relay::connection_lost(const bool cancel_observed)
	{
		return terminate(source_closure_relay_terminal::connection_lost, cancel_observed);
	}

	sdk::result<std::string> source_closure_spool_relay::worker_crashed(const bool cancel_observed)
	{
		return terminate(source_closure_relay_terminal::worker_crashed, cancel_observed);
	}
} // namespace cxxlens::detail::clang22
