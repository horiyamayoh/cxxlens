struct LongOnly
{
	long value;
};
struct Bitfields
{
	signed long signed_value : 7;
	unsigned long unsigned_value : 9;
	unsigned : 0;
	long ordinary;
};
template <unsigned Width>
struct DependentBits
{
	unsigned value : Width;
	long ordinary;
};
int nonfield_variable;
int nonfield_function()
{
	return 0;
}
