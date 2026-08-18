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
		/**
		 * The only filesystem event that aborts the whole materialization task: the source
		 * closure's own manifest claims a project/generated member exists at some logical path,
		 * but the mounted filesystem could not actually serve it once Clang asked for it. Every
		 * other filesystem event `closure_routing_file_system` sees is either real, admitted
		 * content or an ordinary "not found" that Clang's own driver already tolerates (see the
		 * region breakdown in source_closure_native.hpp).
		 */
		class native_vfs_audit final
		{
		  public:
			void record_missing(std::string path)
			{
				std::scoped_lock lock{mutex_};
				if (!missing_)
					missing_ = std::move(path);
			}

			[[nodiscard]] std::optional<std::string> missing_path() const
			{
				std::scoped_lock lock{mutex_};
				return missing_;
			}

		  private:
			mutable std::mutex mutex_;
			std::optional<std::string> missing_;
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

		[[nodiscard]] std::error_code enoent() noexcept
		{
			return std::make_error_code(std::errc::no_such_file_or_directory);
		}

		/**
		 * Compiler-facing filesystem mounted for exactly one Clang invocation. Every lookup is
		 * routed into one of three disjoint regions:
		 *
		 *   - `route::closure` — beneath the closure's synthetic project root. Served from the
		 *     authenticated in-memory closure content. A miss here is recorded by `audit_` and
		 *     turns into an unconditional task failure (see `with_source_closure_translation_unit`
		 *     below) because the closure manifest already claims this member exists.
		 *   - `route::qualified` — beneath one of the admitted `qualified_roots_`, i.e. the exact
		 *     toolchain surface (resource-dir, sysroot, ...) the materializer itself selected and
		 *     pinned for this invocation. Delegated to the real filesystem: real content that
		 *     exists is served, and an ordinary miss is reported exactly as the real filesystem
		 *     reports it — no audit, no task-level effect.
		 *   - `route::denied` — neither of the above. This is where Clang's own speculative,
		 *     distro/toolchain-dependent driver probing lands (GCC installation candidates,
		 *     `/etc/os-release`, and the like): no static allowlist can enumerate that candidate
		 *     set, and Clang already tolerates its absence gracefully. Answered with a plain
		 *     ENOENT, indistinguishable from a path that simply does not exist, and never
		 *     recorded anywhere — real ambient content is never reachable through this route
		 *     (the underlying real filesystem is never consulted for a denied path), so the
		 *     fail-closed guarantee against ambient bytes holds structurally rather than by
		 *     auditing and aborting after the fact.
		 */
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
						auto result = closure_filesystem_->status(*normalized);
						if (!result)
							audit_->record_missing(logical_from_synthetic(*normalized));
						return result;
					}
					case route::qualified:
						return getUnderlyingFS().status(*normalized);
					case route::denied:
						return enoent();
				}
				return enoent();
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
						auto result = closure_filesystem_->openFileForRead(*normalized);
						if (!result)
							audit_->record_missing(logical_from_synthetic(*normalized));
						return result;
					}
					case route::qualified:
						return getUnderlyingFS().openFileForRead(*normalized);
					case route::denied:
						return enoent();
				}
				return enoent();
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
						return closure_filesystem_->dir_begin(*normalized, error);
					case route::qualified:
						return getUnderlyingFS().dir_begin(*normalized, error);
					case route::denied:
						error = enoent();
						return {};
				}
				error = enoent();
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
					return enoent();
				if (auto error = closure_filesystem_->setCurrentWorkingDirectory(*normalized))
					return error;
				working_directory_ = *normalized;
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
						output.assign(normalized->begin(), normalized->end());
						return {};
					case route::qualified:
						return getUnderlyingFS().getRealPath(*normalized, output);
					case route::denied:
						return enoent();
				}
				return enoent();
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
						return getUnderlyingFS().isLocal(*normalized, result);
					case route::denied:
						return enoent();
				}
				return enoent();
			}

		  private:
			enum class route : unsigned char
			{
				closure,
				qualified,
				denied,
			};

			[[nodiscard]] llvm::ErrorOr<std::string> normalize(const llvm::Twine& path) const
			{
				const auto raw = path.str();
				if (raw.find('\0') != std::string::npos || raw.find('\\') != std::string::npos)
					return std::make_error_code(std::errc::invalid_argument);
				const auto relative = raw.empty() || raw.front() != '/';
				llvm::SmallString<256> normalized;
				if (relative)
				{
					normalized.append(working_directory_);
					llvm::sys::path::append(normalized, llvm::sys::path::Style::posix, raw);
				}
				else
				{
					normalized.append(raw);
				}
				llvm::sys::path::remove_dots(normalized, true, llvm::sys::path::Style::posix);
				auto value = llvm::sys::path::convert_to_slash(
					normalized, llvm::sys::path::Style::posix);
				if (value.empty() || value.front() != '/')
					return std::make_error_code(std::errc::invalid_argument);
				return value;
			}

			[[nodiscard]] route classify(const std::string_view path) const noexcept
			{
				if (beneath(path, source_closure_vfs::synthetic_root()))
					return route::closure;
				if (std::ranges::any_of(qualified_roots_,
					[&path](const std::string& root)
					{
						return beneath(path, root);
					}))
					return route::qualified;
				return route::denied;
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
		// A missing project/generated closure member is a determinate input failure (the
		// record's core invariant) and must fail the task even when Clang's own run reports
		// success for the translation unit as a whole — e.g. a `__has_include` guard, or any
		// other construct that tolerates an absent file without diagnosing an error. This check
		// is therefore unconditional, never gated on `!outcome`: gating it on Clang's own
		// success/failure would let an incomplete closure escape detection whenever Clang
		// itself happens not to need the missing member in order to succeed.
		if (const auto missing = mount->audit->missing_path())
			return sdk::unexpected(
				failure("source-closure.member-missing", "compiler-vfs", *missing));
		return outcome;
#else
		(void)main_blob;
		(void)callback;
		return sdk::unexpected(failure(
			"native.unsupported-clang-major", "clang", "22"));
#endif
	}
} // namespace cxxlens::detail::clang22
