// Unknown-bound array patterns preserve their bound through substitution ([temp.deduct.type], #222).
template<class T> struct Unbounded { static constexpr bool value = false; };
template<class T> struct Unbounded<T[]> { static constexpr bool value = true; };
static_assert(Unbounded<int[]>::value);
static_assert(!Unbounded<int>::value);
static_assert(!Unbounded<int[2]>::value);
static_assert(!__is_same(int[], int));
static_assert(!__is_same(int[], int[2]));
void adjusted(int values[]);
using Adjusted = void (*)(int*);
static_assert(__is_same(decltype(&adjusted), Adjusted));
