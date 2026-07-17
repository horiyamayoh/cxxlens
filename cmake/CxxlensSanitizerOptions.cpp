// Sanitizer child processes run with a deliberately minimal execve environment.
// Runtime defaults keep the same fail-closed policy without weakening that boundary.
extern "C" const char* __asan_default_options()
{
	return "detect_leaks=1:halt_on_error=1:exitcode=86:handle_segv=0:symbolize=1";
}

extern "C" const char* __ubsan_default_options()
{
	return "halt_on_error=1:exitcode=86:print_stacktrace=1:symbolize=1";
}

extern "C" const char* __tsan_default_options()
{
	return "halt_on_error=1:exitcode=86:handle_segv=0:second_deadlock_stack=1:symbolize=1";
}
