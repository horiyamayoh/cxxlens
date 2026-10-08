int global_object;
thread_local int thread_object;
int scalar_return(int input)
{
	return input + 0;
}
void void_return()
{
	return;
}
int* local_address()
{
	int object = 1;
	return &object;
}
const int& local_reference()
{
	int object = 1;
	return object;
}
int* parenthesized_address()
{
	int object = 1;
	return ((&object));
}
const int* qualified_address()
{
	int object = 1;
	return &object;
}
int* local_array()
{
	int objects[2]{};
	return objects;
}
int* parameter_address(int object)
{
	return &object;
}
int* reference_parameter(int& object)
{
	return &object;
}
int* pointer_value(int* object)
{
	return object;
}
int* explicit_cast_address()
{
	int object = 1;
	return reinterpret_cast<int*>(&object);
}
int* global_address()
{
	return &global_object;
}
int* static_address()
{
	static int object = 1;
	return &object;
}
int* thread_address()
{
	return &thread_object;
}
struct record
{
	int member;
};
int* member_address(record& object)
{
	return &object.member;
}
int* conditional_address(bool selected)
{
	int object = 1;
	return selected ? &object : &global_object;
}
int* branch_address(bool selected)
{
	int object = 1;
	if (selected)
		return &object;
	return &object;
}
const int* captured_copy()
{
	int object = 1;
	auto closure = [object]
	{
		return &object;
	};
	return closure();
}
int* captured_reference()
{
	int object = 1;
	auto closure = [&object]
	{
		return &object;
	};
	return closure();
}
template <class T>
T* dependent_return(T* object)
{
	return object;
}
int* default_source(int* object = &global_object);
int* default_call()
{
	return default_source();
}
int* bodyless(int*);
