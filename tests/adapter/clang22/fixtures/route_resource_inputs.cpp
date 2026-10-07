// One original TU for the route/resource adapter oracle. No system headers.
void risky();
void safe() noexcept;
struct Guard
{
	Guard();
	~Guard() noexcept(false);
};
void direct_boundary() noexcept
{
	risky();
}
void cleanup_boundary()
{
	Guard guard;
	risky();
}
void caught_boundary() noexcept
{
	try
	{
		risky();
	}
	catch (...)
	{
	}
}
extern "C" void user_termination() noexcept __attribute__((noreturn));
void caught_then_termination() noexcept
{
	try
	{
		risky();
	}
	catch (...)
	{
		user_termination();
	}
}
void lambda_boundary()
{
	auto nested = []() noexcept
	{
		risky();
	};
	nested();
}
unsigned long trait_type()
{
	return sizeof(char);
}
unsigned long trait_expression(int argument)
{
	return sizeof(argument);
}
unsigned long trait_alignment()
{
	return alignof(int);
}
template <class T>
unsigned long trait_dependent()
{
	return sizeof(T);
}
_Atomic(int) atomic_value;
int atomic_read()
{
	return __c11_atomic_load(&atomic_value, 2);
}
bool atomic_exchange(int& expected)
{
	return __c11_atomic_compare_exchange_strong(&atomic_value, &expected, 1, 5, 2);
}
void atomic_initialize()
{
	__c11_atomic_init(&atomic_value, 0);
}
int atomic_unknown_order(int order)
{
	return __c11_atomic_load(&atomic_value, order);
}
int ordinary_builtin(int argument)
{
	return __builtin_abs(argument);
}
int lifetime_end_probe()
{
	int local = 1;
	return local;
}
namespace std
{
	template <class Promise = void>
	struct coroutine_handle
	{
		static coroutine_handle from_address(void*) noexcept
		{
			return {};
		}
		operator coroutine_handle<void>() const noexcept
		{
			return {};
		}
	};
	template <class Return, class... Args>
	struct coroutine_traits
	{
		using promise_type = typename Return::promise_type;
	};
} // namespace std
struct Awaiter
{
	bool await_ready() noexcept
	{
		return false;
	}
	void await_suspend(std::coroutine_handle<>) noexcept {}
	void await_resume() noexcept {}
};
struct Task
{
	struct promise_type
	{
		Task get_return_object()
		{
			return {};
		}
		Awaiter initial_suspend()
		{
			return {};
		}
		Awaiter final_suspend() noexcept
		{
			return {};
		}
		void return_void() noexcept {}
		void unhandled_exception() noexcept {}
	};
};
Task named_coroutine()
{
	co_await Awaiter{};
	co_return;
}
void lambda_coroutine()
{
	auto nested = []() -> Task
	{
		co_await Awaiter{};
		co_return;
	};
	(void)nested;
}
int plain_char_probe(char value)
{
	return value;
}
int signed_char_control(signed char value)
{
	return value;
}
int unsigned_char_control(unsigned char value)
{
	return value;
}
void path_sink(const char*);
void path_probe()
{
	path_sink("a/b");
}
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

enum ObservedRange
{
	range_first = 1,
	range_last = 3
};
ObservedRange enum_conversion_probe()
{
	return static_cast<ObservedRange>(4);
}
bool bool_representation_probe()
{
	return __builtin_bit_cast(bool, static_cast<unsigned char>(2));
}
float direct_type_access_probe()
{
	int object = 0;
	return *reinterpret_cast<float*>(&object);
}
int missing_return_probe() {}

// Explicit consumer contract supplies format semantics; names do not.
void format_probe_sink(const char* format, ...);
void format_invalid_argument_probe()
{
	format_probe_sink("%d", 1.0);
}
void format_integer_control()
{
	format_probe_sink("%d", 1);
}
void format_promotion_control(float value)
{
	format_probe_sink("%f", value);
}

// Actual warning-only checker candidate and a successful original evaluation root.
int original_sequence_probe()
{
	int value = 0;
	return value++ + value++;
}
union OriginalUnion
{
	int active;
	double alternate;
};
constexpr int original_live_union()
{
	OriginalUnion value{1};
	value.alternate = 2;
	return int(value.alternate);
}
constexpr int original_live_union_result = original_live_union();
