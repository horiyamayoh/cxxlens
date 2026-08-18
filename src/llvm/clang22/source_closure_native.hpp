#pragma once

#include <string>
#include <vector>

#include <cxxlens/provider/clang22.hpp>

#include "source_closure.hpp"

namespace cxxlens::detail::clang22
{
	/** Source-private native execution input for one authenticated source closure. */
	struct source_closure_native_input
	{
		source_closure_snapshot closure;
		std::string main_logical_path;
		std::string logical_working_directory;
		std::vector<std::string> effective_arguments;
		std::vector<std::string> qualified_read_roots;
	};

	/**
	 * Execute one fresh Clang job through a closure-exclusive synthetic project filesystem.
	 *
	 * The mounted compiler-facing filesystem serves three disjoint regions:
	 *   1. The source closure's own project/generated members (authoritative; a member the
	 *      closure claims to have but cannot actually serve is a hard, unconditional failure —
	 *      see `source-closure.member-missing` below).
	 *   2. The admitted qualified toolchain root(s) named by `qualified_read_roots` — the exact
	 *      toolchain surface the materializer itself selected and pinned. Real content that
	 *      exists under an admitted root is served; content that does not exist there is an
	 *      ordinary miss, exactly as the qualified filesystem already reports it.
	 *   3. Everything else. Clang's own driver performs many speculative, distro/toolchain
	 *      dependent probes outside both surfaces above (GCC installation candidates,
	 *      `/etc/os-release`, and similar) that Clang tolerates gracefully on ENOENT. Such
	 *      probes see a clean, ordinary "no such file" here — never a task-level failure — since
	 *      no static allowlist can enumerate Clang's full candidate set and the driver already
	 *      handles their absence correctly.
	 *
	 * A missing closure member (region 1) is always a determinate input failure and is reported
	 * unconditionally, independent of whether Clang's own overall run reports success or failure
	 * for the translation unit (e.g. a `__has_include` guard can make Clang tolerate the same
	 * absence that must still fail this task).
	 */
	[[nodiscard]] sdk::result<void> with_source_closure_translation_unit(
		const source_closure_native_input& input,
		provider::clang22::translation_unit_callback callback);
} // namespace cxxlens::detail::clang22
