#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/provider/clang22.hpp>

#include "source_closure.hpp"

namespace cxxlens::detail::clang22
{
	/**
	 * Optional policy port for the compiler-facing mount. A rejected member remains claimed by
	 * the authenticated closure and therefore must fail closed as `source-closure.member-missing`.
	 * The normal production path leaves this port empty and mounts every validated member.
	 */
	struct source_closure_member_mount_policy
	{
		using callback_type = bool (*)(std::string_view logical_path, void* context) noexcept;

		callback_type should_mount{};
		void* context{};

		[[nodiscard]] bool admits(const std::string_view logical_path) const noexcept
		{
			return should_mount == nullptr || should_mount(logical_path, context);
		}
	};

	/** Source-private native execution input for one authenticated source closure. */
	struct source_closure_native_input
	{
		source_closure_snapshot closure;
		std::string main_logical_path;
		std::string logical_working_directory;
		std::vector<std::string> effective_arguments;
		/**
		 * Absolute toolchain roots admitted for this invocation. These are trusted verbatim:
		 * nothing in this unit verifies that they correspond to a toolchain the materializer
		 * actually selected, pinned, or measured -- choosing them is the caller's security
		 * responsibility. Note also that reads beneath an admitted root are delegated to the
		 * real filesystem, which follows symlinks, so an admitted root bounds the *names* a
		 * lookup may use, not the *bytes* those names ultimately resolve to.
		 */
		std::vector<std::string> qualified_read_roots;
		source_closure_member_mount_policy member_mount_policy;
	};

	/**
	 * Execute one fresh Clang job through a closure-exclusive synthetic project filesystem.
	 *
	 * The mounted compiler-facing filesystem serves three disjoint regions:
	 *   1. The source closure's own project/generated members, served authoritatively from
	 *      authenticated in-memory content.
	 *   2. The admitted toolchain root(s) named by `qualified_read_roots` (see the caveats on
	 *      that field). Real content that exists under an admitted root is served; content that
	 *      does not exist there is an ordinary miss.
	 *   3. Everything else, including paths beneath the synthetic project root that are not
	 *      closure members. Clang's driver and preprocessor both probe speculatively -- GCC
	 *      installation candidates and `/etc/os-release` outside the project root, and inside it
	 *      the includer-directory-first rule for quoted includes, each `-I`/`-iquote` entry in
	 *      search order, and `__has_include`. All of these routinely name paths that are not
	 *      closure members even when the closure is complete, and Clang tolerates their absence
	 *      gracefully. Such probes see a clean, ordinary "no such file" here.
	 *
	 * The single filesystem event that fails the task is a member the closure's manifest
	 * actually claims which the mounted filesystem could not serve. That is a determinate input
	 * failure (`source-closure.member-missing`) and is reported unconditionally, independent of
	 * whether Clang's own run reports success -- a construct such as `__has_include` can make
	 * Clang tolerate the very absence that must still fail this task. A closure that is merely
	 * *incomplete* relative to what the source needs is not this case: Clang reports its own
	 * ordinary "file not found" error and the task fails through `native.parse-failed`.
	 *
	 * Ordering contract: the callback runs *inside* the Clang invocation, before the
	 * closure-completeness verdict is consulted. It can therefore have already executed, and
	 * already produced output, on a call that ultimately returns a failure. Callers must treat
	 * everything the callback emits as provisional until this function returns success, and must
	 * discard it otherwise; a successful callback never overrides a failed verdict here.
	 */
	[[nodiscard]] sdk::result<void>
	with_source_closure_translation_unit(const source_closure_native_input& input,
										 provider::clang22::translation_unit_callback callback);
} // namespace cxxlens::detail::clang22
