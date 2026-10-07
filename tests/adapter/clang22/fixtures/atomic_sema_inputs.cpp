_Atomic(int) global_atomic;
int global_observation = __c11_atomic_load(&global_atomic, __ATOMIC_RELAXED);

void orders(_Atomic(int)* pointer)
{
	__c11_atomic_fetch_add(pointer, 1, __ATOMIC_RELAXED);
	__c11_atomic_fetch_add(pointer, 1, __ATOMIC_CONSUME);
	__c11_atomic_fetch_add(pointer, 1, __ATOMIC_ACQUIRE);
	__c11_atomic_fetch_add(pointer, 1, __ATOMIC_RELEASE);
	__c11_atomic_fetch_add(pointer, 1, __ATOMIC_ACQ_REL);
	__c11_atomic_fetch_add(pointer, 1, __ATOMIC_SEQ_CST);
}
int loaded(_Atomic(int)* pointer)
{
	return __c11_atomic_load(pointer, __ATOMIC_ACQUIRE);
}
void stored(_Atomic(int)* pointer)
{
	__c11_atomic_store(pointer, 1, __ATOMIC_RELEASE);
}
int exchanged(_Atomic(int)* pointer)
{
	return __c11_atomic_exchange(pointer, 1, __ATOMIC_ACQ_REL);
}
bool compared(_Atomic(int)* pointer, int* expected)
{
	return __c11_atomic_compare_exchange_strong(
		pointer, expected, 1, __ATOMIC_RELAXED, __ATOMIC_SEQ_CST);
}
bool invalid_failure(_Atomic(int)* pointer, int* expected)
{
	return __c11_atomic_compare_exchange_strong(
		pointer, expected, 1, __ATOMIC_SEQ_CST, __ATOMIC_RELEASE);
}
bool tested(char* pointer)
{
	return __atomic_test_and_set(pointer, __ATOMIC_ACQUIRE);
}
void cleared(char* pointer)
{
	__atomic_clear(pointer, __ATOMIC_RELEASE);
}
void initialized(_Atomic(int)* pointer)
{
	__c11_atomic_init(pointer, 1);
}
int dynamic_order(_Atomic(int)* pointer, int order)
{
	return __c11_atomic_load(pointer, order);
}
int invalid_load(_Atomic(int)* pointer)
{
	return __c11_atomic_load(pointer, __ATOMIC_RELEASE);
}
void invalid_store(_Atomic(int)* pointer)
{
	__c11_atomic_store(pointer, 1, __ATOMIC_ACQUIRE);
}
void invalid_clear(char* pointer)
{
	__atomic_clear(pointer, __ATOMIC_CONSUME);
}
unsigned unevaluated(_Atomic(int)* pointer)
{
	return sizeof(__c11_atomic_load(pointer, __ATOMIC_RELAXED));
}
int discarded(_Atomic(int)* pointer)
{
	if constexpr (false)
		return __c11_atomic_load(pointer, __ATOMIC_RELAXED);
	return 0;
}
int default_context(int value = __c11_atomic_load(&global_atomic, __ATOMIC_RELAXED))
{
	return value;
}
int local_value()
{
	int value = __c11_atomic_load(&global_atomic, __ATOMIC_RELAXED);
	return value;
}
void nested_owner()
{
	auto nested = []
	{
		return __c11_atomic_load(&global_atomic, __ATOMIC_RELAXED);
	};
	(void)nested;
}
