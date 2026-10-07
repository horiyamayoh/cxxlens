void ordinary(int);
namespace user
{
	void __atomic_thread_fence(int);
} // namespace user
void sites(int dynamic, void (*indirect)(int))
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	__atomic_signal_fence(dynamic);
	__c11_atomic_thread_fence(__ATOMIC_RELAXED);
	__c11_atomic_signal_fence(-1);
	__scoped_atomic_thread_fence(__ATOMIC_ACQUIRE, __MEMORY_SCOPE_SYSTEM);
	ordinary(dynamic);
	indirect(dynamic);
	user::__atomic_thread_fence(dynamic);
	(void)__builtin_expect(dynamic, 1);
}
int global = (__atomic_thread_fence(__ATOMIC_SEQ_CST), 0);
void default_site(int = (__atomic_signal_fence(__ATOMIC_SEQ_CST), 0));
void discarded()
{
	if constexpr (false)
		__atomic_thread_fence(5);
}
int unevaluated()
{
	return sizeof((__atomic_signal_fence(5), 0));
}
void nested()
{
	[]
	{
		__c11_atomic_thread_fence(5);
	}();
}
