int helper(int);
int scalar_zero(int input)
{
	int result = input + 1;
	return result + 0;
}
enum plain_enum
{
	plain_value
};
int enum_zero(plain_enum value)
{
	return value == plain_value;
}
int call_candidate(int input)
{
	return helper(input);
}
int pointer_candidate(int* pointer)
{
	return *pointer;
}
int reference_candidate(int& reference)
{
	return reference;
}
struct record
{
	int value;
};
int record_candidate(record object)
{
	return object.value;
}
int array_candidate()
{
	int values[2]{1, 2};
	return values[0];
}
int* address_candidate(int& value)
{
	return &value;
}
int default_source(int input = helper(0));
int default_candidate()
{
	return default_source();
}
struct default_field
{
	int* pointer = nullptr;
	default_field() {}
};
int assembly_candidate()
{
	asm("");
	return 0;
}
template <class T>
T dependent_candidate(T value)
{
	return value;
}
int absent_body(int*);
int* global_pointer;

int xvalue_cast_candidate(int value)
{
	static_cast<int&&>(value);
	return value;
}
int reference_cast_candidate(int value)
{
	static_cast<int&>(value);
	return value;
}
int cstyle_xvalue_candidate(int value)
{
	(int&&)value;
	return value;
}
int reference_capture_candidate(int value)
{
	auto closure = [&value]
	{
		return value;
	};
	return value;
}

int implicit_move_candidate(int input)
{
	int result = input + 1;
	return result;
}
