#pragma once

namespace cxxlens::test
{
	/**
	 * Run the #202 crash/recrash scenario against a fresh temporary SQLite database.
	 *
	 * The helper is intentionally test-only.  Its VFS delegates every operation to SQLite's
	 * selected native VFS.  A journal-sync pause is admitted only after the on-disk SQLite header,
	 * records, pager checksums, preimages, and captured database identity agree.  It does not
	 * manufacture, edit, or replay a journal, and it does not pass an operation receipt between the
	 * crashed processes.
	 */
	[[nodiscard]] bool run_sqlite202_crash_recrash(const char* executable_path) noexcept;

	/** Dispatch the private child modes used by run_sqlite202_crash_recrash. */
	[[nodiscard]] int sqlite202_crash_child_entry(int argc, char** argv) noexcept;
} // namespace cxxlens::test
