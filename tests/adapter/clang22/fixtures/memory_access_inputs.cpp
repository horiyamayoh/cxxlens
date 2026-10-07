// Original builtin access admission and storage controls; no system headers.
struct member_object
{
	int value;
};
int array_indices(int index)
{
	int values[3]{};
	return values[-1] + values[0] + values[2] + values[3] + values[index];
}
void array_addresses()
{
	int values[3]{};
	(void)&values[3];
	(void)&values[4];
}
int* cancelled_dereference(int* pointer)
{
	return &((*pointer));
}
int pointer_dereference(int* pointer)
{
	return *pointer;
}
int arrow_access(member_object* pointer)
{
	return pointer->value;
}
int pointer_subscript(int* pointer, int index)
{
	return pointer[index];
}
void nested_address()
{
	int values[2][3]{};
	(void)&values[1][3];
}
int sizeof_access(int* pointer)
{
	return sizeof(*pointer);
}
int decltype_access(int* pointer)
{
	using reference = decltype(*pointer);
	return 0;
}
struct overloaded_access
{
	int operator[](int);
	int operator*();
};
int overloaded_control(overloaded_access& object)
{
	return object[0] + *object;
}
template <class T>
int dependent_access(T& object)
{
	return object[0];
}
extern int global_array[3];
int default_index(int value = global_array[4]);
int default_activation()
{
	return default_index();
}
int nested_default(int value = default_index());
int nested_default_activations()
{
	return nested_default() + nested_default();
}
struct default_field
{
	int* pointer = nullptr;
	int value = *pointer;
	default_field();
};
default_field::default_field() {}
int healthy_sibling(int value)
{
	return value + 1;
}
int absent_body(int* pointer);
enum storage_enum : unsigned short
{
	storage_first = 1
};
using bit_storage = unsigned _BitInt(9);
unsigned long storage_units()
{
	return sizeof(bit_storage) + sizeof(storage_enum) + sizeof(bool);
}
bit_storage storage_identity(bit_storage value)
{
	return value;
}
