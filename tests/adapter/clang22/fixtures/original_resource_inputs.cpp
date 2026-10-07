// Original native admission fixture; no standard library spelling is a classifier.
struct moved
{
	moved();
	moved(const moved& source);
	moved(moved&& source);
	moved& operator=(const moved& source);
	moved& operator=(moved&& source);
};
template <class T>
T&& as_rvalue(T& value)
{
	return static_cast<T&&>(value);
}
void selected_members(moved& source, moved& second)
{
	moved from_move(as_rvalue(source));
	moved from_copy(second);
	from_copy = as_rvalue(second);
	from_move.operator=(source);
	// An xvalue expression/rr binding alone is not a selected special-member move.
	moved&& xvalue_only = as_rvalue(source);
	(void)xvalue_only;
}
struct copy_only
{
	copy_only(const copy_only& source);
};
void selected_copy_despite_xvalue(copy_only& source)
{
	copy_only copy(as_rvalue(source));
}
__attribute__((ownership_returns(heap))) void* acquire();
__attribute__((ownership_returns(heap, 1))) void* acquire_sized(unsigned long size);
__attribute__((ownership_takes(heap, 1, 2))) void consume(void* first, void* second);
__attribute__((ownership_holds(heap, 1))) void hold(void* object);
__attribute__((annotate("arbitrary annotation", 11, 22))) void uninterpreted(void* object);
struct ownership_methods
{
	__attribute__((ownership_takes(heap, 2, 3))) void consume(void* first, void* second);
};
__attribute__((deprecated("ordinary attribute remains in census"))) void ordinary();
void no_attributes(void* object);
int global_value;
int* global_pointer = &global_value;
struct destinations
{
	int member_value;
	int* pointer = &member_value;
	int& reference;
	destinations(int& value) : reference(value) {}
	void store(int& local)
	{
		pointer = &local;
	}
};
int* return_address()
{
	int local;
	return &local;
}
int& return_reference(int& value)
{
	return value;
}
void address_routes(int& external)
{
	int local;
	int local_array[2];
	int* array_decay = local_array;
	void (*function_decay)() = ordinary;
	int* direct_address = &local;
	int* plain_pointer_copy = direct_address;
	int& direct_reference = local;
	static int* static_pointer = &external;
	auto closures = [&local, copied = direct_address, &external]
	{
		return copied;
	};
	global_pointer = direct_address;
	(void)array_decay;
	(void)function_decay;
	(void)plain_pointer_copy;
	(void)direct_reference;
	(void)static_pointer;
	(void)closures;
}
template <class T>
void unresolved_constructor(T&& value)
{
	T candidate(as_rvalue(value));
	(void)T(as_rvalue(value));
}
