#include "source_closure_native.hpp"

#include "provider_sdk_internal.hpp"
#include "source_closure_invocation.hpp"
#include "source_closure_vfs.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if CXXLENS_HAS_CLANG22
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/VirtualFileSystem.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] sdk::error failure(std::string code,
									   std::string field,
									   std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

#if CXXLENS_HAS_CLANG22
		enum class native_vfs_failure_kind : unsigned char
		{
			member_missing,
			ambient_fallback_denied,
			toolchain_input_unqualified,
		};

		struct native_vfs_failure
		{
			native_vfs_failure_kind kind{native_vfs_failure_kind::member_missing};
			std::string path;
		};

		class native_vfs_audit final
		{
		  public:
			void record_missing(std::string path)
			{
				std::scoped_lock lock{mutex_};
				if (!missing_)
					missing_ = native_vfs_failure{
						native_vfs_failure_kind::member_missing, std::move(path)};
			}

			void record_policy(native_vfs_failure_kind kind, std::string path)
			{
				std::scoped_lock lock{mutex_};
				if (!policy_)
					policy_ = native_vfs_failure{kind, std::move(path)};
			}

			[[nodiscard]] std::optional<native_vfs_failure> policy_failure() const
			{
				std::scoped_lock lock{mutex_};
				return policy_;
			}

			[[nodiscard]] std::optional<native_vfs_failure> missing_failure() const
			{
				std::scoped_lock lock{mutex_};
				return missing_;
			}

		  private:
			mutable std::mutex mutex_;
			std::optional<native_vfs_failure> policy_;
			std::optional<native_vfs_failure> missing_;
		};

		struct normalized_native_path
		{
			std::string value;
			bool relative{};
			bool closure_spelling{};
		};

		[[nodiscard]] bool beneath(const std::string_view path,
							   const std::string_view root) noexcept
		{
			return path == root ||
				(path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/');
		}

		[[nodiscard]] std::string logical_from_synthetic(const std::string_view path)
		{
			const auto root = source_closure_vfs::synthetic_root();
			if (path == root)
				return "project://";
			return "project://" + std::string{path.substr(root.size() + 1U)};
		}

		class closure_routing_file_system final : public llvm::vfs::ProxyFileSystem
		{
		  public:
			closure_routing_file_system(
				llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> qualified_filesystem,
				llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> closure_filesystem,
				std::vector<std::string> qualified_roots,
				std::string working_directory,
				std::shared_ptr<native_vfs_audit> audit)
				: ProxyFileSystem{std::move(qualified_filesystem)},
				  closure_filesystem_{std::move(closure_filesystem)},
				  qualified_roots_{std::move(qualified_roots)},
				  working_directory_{std::move(working_directory)},
				  audit_{std::move(audit)}
			{
			}

			llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine& path) override
			{
				auto normalized = normalize(path);
				if (!normalized)
					return normalized.getError();
				switch (classify(*normalized))
				{
					case route::closure:
					{
						auto result = closure_filesystem_->status(normalized->value);
						if (!result)
							audit_->record_missing(logical_from_synthetic(normalized->value));
						return result;
					}
					case route::qualified:
						return getUnderlyingFS().status(normalized->value);
					case route::denied:
						return deny(*normalized);
				}
				return std::make_error_code(std::errc::permission_denied);
			}

			bool exists(const llvm::Twine& path) override
			{
				return static_cast<bool>(status(path));
			}

			llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
			openFileForRead(const llvm::Twine& path) override
			{
				auto normalized = normalize(path);
				if (!normalized)
					return normalized.getError();
				switch (classify(*normalized))
				{
					case route::closure:
					{
						auto result = closure_filesystem_->openFileForRead(normalized->value);
						if (!result)
							audit_->record_missing(logical_from_synthetic(normalized->value));
						return result;
					}
					case route::qualified:
						return getUnderlyingFS().openFileForRead(normalized->value);
					case route::denied:
						return deny(*normalized);
				}
				return std::make_error_code(std::errc::permission_denied);
			}

			llvm::vfs::directory_iterator dir_begin(const llvm::Twine& path,
													 std::error_code& error) override
			{
				auto normalized = normalize(path);
				if (!normalized)
				{
					error = normalized.getError();
					return {};
				}
				switch (classify(*normalized))
				{
					case route::closure:
						return closure_filesystem_->dir_begin(normalized->value, error);
					case route::qualified:
						return getUnderlyingFS().dir_begin(normalized->value, error);
					case route::denied:
						error = deny(*normalized);
						return {};
				}
				error = std::make_error_code(std::errc::permission_denied);
				return {};
			}

			llvm::ErrorOr<std::string> getCurrentWorkingDirectory() const override
			{
				return working_directory_;
			}

			std::error_code setCurrentWorkingDirectory(const llvm::Twine& path) override
			{
				auto normalized = normalize(path);
				if (!normalized)
					return normalized.getError();
				if (classify(*normalized) != route::closure)
					return deny(*normalized);
				if (auto error = closure_filesystem_->setCurrentWorkingDirectory(
						normalized->value))
					return error;
				working_directory_ = std::move(normalized->value);
				return {};
			}

			std::error_code getRealPath(const llvm::Twine& path,
										 llvm::SmallVectorImpl<char>& output) override
			{
				auto normalized = normalize(path);
				if (!normalized)
					return normalized.getError();
				switch (classify(*normalized))
				{
					case route::closure:
						output.assign(normalized->value.begin(), normalized->value.end());
						return {};
					case route::qualified:
						return getUnderlyingFS().getRealPath(normalized->value, output);
					case route::denied:
						return deny(*normalized);
				}
				return std::make_error_code(std::errc::permission_denied);
			}

			std::error_code isLocal(const llvm::Twine& path, bool& result) override
			{
				auto normalized = normalize(path);
				if (!normalized)
					return normalized.getError();
				switch (classify(*normalized))
				{
					case route::closure:
						result = true;
						return {};
					case route::qualified:
						return getUnderlyingFS().isLocal(normalized->value, result);
					case route::denied:
						return deny(*normalized);
				}
				return std::make_error_code(std::errc::permission_denied);
			}

		  private:
			enum class route : unsigned char
			{
				closure,
				qualified,
				denied,
			};

			[[nodiscard]] llvm::ErrorOr<normalized_native_path>
			normalize(const llvm::Twine& path) const
			{
				const auto raw = path.str();
				if (raw.find('\0') != std::string::npos || raw.find('\\') != std::string::npos)
					return std::make_error_code(std::errc::invalid_argument);
				normalized_native_path output;
				output.relative = raw.empty() || raw.front() != '/';
				output.closure_spelling = raw == source_closure_vfs::synthetic_root() ||
					raw.starts_with(
						std::string{source_closure_vfs::synthetic_root()} + "/");
				llvm::SmallString<256> normalized;
				if (output.relative)
				{
					normalized.append(working_directory_);
					llvm::sys::path::append(
						normalized, llvm::sys::path::Style::posix, raw);
				}
				else
				{
					normalized.append(raw);
				}
				llvm::sys::path::remove_dots(
					normalized, true, llvm::sys::path::Style::posix);
				output.value = llvm::sys::path::convert_to_slash(
					normalized, llvm::sys::path::Style::posix);
				if (output.value.empty() || output.value.front() != '/')
					return std::make_error_code(std::errc::invalid_argument);
				return output;
			}

			[[nodiscard]] route classify(const normalized_native_path& path) const noexcept
			{
				if (beneath(path.value, source_closure_vfs::synthetic_root()))
					return route::closure;
				if (std::ranges::any_of(qualified_roots_,
					[&path](const std::string& root)
					{
						return beneath(path.value, root);
					}))
					return route::qualified;
				return route::denied;
			}

			[[nodiscard]] std::error_code deny(const normalized_native_path& path) const
			{
				if (path.relative || path.closure_spelling)
					audit_->record_policy(
						native_vfs_failure_kind::ambient_fallback_denied, path.value);
				else
					audit_->record_policy(
						native_vfs_failure_kind::toolchain_input_unqualified, path.value);
				return std::make_error_code(std::errc::permission_denied);
			}

			llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> closure_filesystem_;
			std::vector<std::string> qualified_roots_;
			std::string working_directory_;
			std::shared_ptr<native_vfs_audit> audit_;
		};

		struct native_vfs_mount
		{
			llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> filesystem;
			std::shared_ptr<native_vfs_audit> audit;
		};

		[[nodiscard]] sdk::result<native_vfs_mount> mount_native_vfs(
			const source_closure_snapshot& closure,
			const source_closure_invocation& invocation)
		{
			if (auto valid = closure.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
			for (const auto& member : closure.members)
			{
				const auto* blob = closure.find_blob(member.content_digest);
				if (blob == nullptr || !blob->content)
					return sdk::unexpected(failure(
						"source-closure.blob-missing", member.logical_path));
				auto relative = source_closure_relative_path(member.logical_path);
				if (!relative)
					return sdk::unexpected(std::move(relative.error()));
				const auto synthetic =
					std::string{source_closure_vfs::synthetic_root()} + "/" + *relative;
				if (!memory->addFile(
						synthetic,
						0,
						llvm::MemoryBuffer::getMemBufferCopy(
							llvm::StringRef{blob->content->data(), blob->content->size()},
							synthetic)))
					return sdk::unexpected(failure(
						"source-closure.digest-mismatch", member.logical_path));
			}
			if (auto error = memory->setCurrentWorkingDirectory(invocation.working_directory))
				return sdk::unexpected(failure(
					"source-closure.path-invalid",
					"working-directory",
					error.message()));

			auto audit = std::make_shared<native_vfs_audit>();
			auto routed = llvm::makeIntrusiveRefCnt<closure_routing_file_system>(
				llvm::vfs::getRealFileSystem(),
				memory,
				invocation.qualified_read_roots,
				invocation.working_directory,
				audit);
			return native_vfs_mount{std::move(routed), std::move(audit)};
		}

		[[nodiscard]] sdk::error audit_failure(const native_vfs_failure& value)
		{
			switch (value.kind)
			{
				case native_vfs_failure_kind::member_missing:
					return failure("source-closure.member-missing", "compiler-vfs", value.path);
				case native_vfs_failure_kind::ambient_fallback_denied:
					return failure(
						"source-closure.ambient-fallback-denied", "compiler-vfs", value.path);
				case native_vfs_failure_kind::toolchain_input_unqualified:
					return failure(
						"source-closure.toolchain-input-unqualified", "compiler-vfs", value.path);
			}
			return failure("source-closure.ambient-fallback-denied", "compiler-vfs");
		}
#endif
	} // namespace

	sdk::result<void> with_source_closure_translation_unit(
		const source_closure_native_input& input,
		provider::clang22::translation_unit_callback callback)
	{
		if (auto valid = input.closure.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		const auto* main = input.closure.find_member(input.main_logical_path);
		if (main == nullptr || main->role != source_closure_role::main)
			return sdk::unexpected(failure(
				"source-closure.main-invalid", "main-logical-path", input.main_logical_path));
		const auto* main_blob = input.closure.find_blob(main->content_digest);
		if (main_blob == nullptr || !main_blob->content)
			return sdk::unexpected(failure(
				"source-closure.blob-missing", "main-logical-path", input.main_logical_path));
		auto invocation = prepare_source_closure_invocation(
			input.effective_arguments,
			input.main_logical_path,
			input.logical_working_directory,
			input.qualified_read_roots);
		if (!invocation)
			return sdk::unexpected(std::move(invocation.error()));
		if (!callback)
			return sdk::unexpected(failure("native.input-invalid", "callback"));

#if CXXLENS_HAS_CLANG22
		auto mount = mount_native_vfs(input.closure, *invocation);
		if (!mount)
			return sdk::unexpected(std::move(mount.error()));
		provider::clang22::translation_unit_input native_input{
			input.closure.snapshot_id,
			main->file_id,
			main->logical_path,
			*main_blob->content,
			input.effective_arguments,
		};
		auto outcome = provider::clang22::detail::with_translation_unit_vfs(
			native_input,
			invocation->compiler_filename,
			invocation->tool_name,
			invocation->compiler_arguments,
			mount->filesystem,
			std::move(callback));
		if (const auto policy = mount->audit->policy_failure())
			return sdk::unexpected(audit_failure(*policy));
		if (!outcome)
		{
			if (const auto missing = mount->audit->missing_failure())
				return sdk::unexpected(audit_failure(*missing));
			return outcome;
		}
		return {};
#else
		(void)main_blob;
		(void)callback;
		return sdk::unexpected(failure(
			"native.unsupported-clang-major", "clang", "22"));
#endif
	}
} // namespace cxxlens::detail::clang22
