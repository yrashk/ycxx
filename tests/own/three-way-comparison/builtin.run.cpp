// [expr.spaceship]: the built-in three-way comparison of arithmetic, enumeration and pointer
// operands yields a comparison category type of <compare> ([cmp.categories]).
#include "inputs/compare.hpp"

template<class A, class B> constexpr bool same = false;
template<class A> constexpr bool same<A, A> = true;

enum E { e0, e1, e2 };
enum class S : unsigned char { a, b };

// Integral operands, after the usual arithmetic conversions: std::strong_ordering.
static_assert(same<decltype(1 <=> 2), std::strong_ordering>);
static_assert(same<decltype('a' <=> 2L), std::strong_ordering>);
static_assert(same<decltype(true <=> false), std::strong_ordering>);
static_assert((1 <=> 2) == std::strong_ordering::less);
static_assert((2 <=> 2) == std::strong_ordering::equal);
static_assert((3 <=> 2) == std::strong_ordering::greater);
static_assert((1 <=> 2) < 0 && (2 <=> 1) > 0 && (2 <=> 2) == 0);
static_assert((-1 <=> 0L) < 0);
static_assert((1u <=> 2u) < 0);
static_assert((1 <=> 1u) == 0);  // a constant that fits is not narrowed
// [expr.spaceship]: between the shifts and the relational operators.
static_assert(1 <=> 2 < 0 && 2 <=> 2 == 0);
static_assert((1 << 1 <=> 2) == 0 && (1 + 1 <=> 2) == 0);

// Floating operands: std::partial_ordering, unordered for a NaN.
static_assert(same<decltype(1.0 <=> 2.0), std::partial_ordering>);
static_assert(same<decltype(1 <=> 2.0f), std::partial_ordering>);  // integral to floating
static_assert((1.0 <=> 2.0) == std::partial_ordering::less);
static_assert((2.0 <=> 2) == std::partial_ordering::equivalent);
static_assert((2.5f <=> 2.0) == std::partial_ordering::greater);
static_assert((__builtin_nan("") <=> 1.0) == std::partial_ordering::unordered);

// Enumerations: an unscoped one by its promoted value, a scoped one by its underlying type.
static_assert(same<decltype(e0 <=> e1), std::strong_ordering>);
static_assert((e0 <=> e1) < 0 && (e2 <=> 1) > 0 && (1 <=> e1) == 0);
static_assert(same<decltype(S::a <=> S::b), std::strong_ordering>);
static_assert((S::a <=> S::b) < 0 && (S::b <=> S::b) == 0);

// Pointers: converted to their composite pointer type.
int array[3];
static_assert(same<decltype(&array[0] <=> &array[1]), std::strong_ordering>);
static_assert((&array[0] <=> &array[2]) < 0);
static_assert((array + 2 <=> array + 1) > 0);
static_assert((array <=> &array[0]) == 0);  // array-to-pointer
constexpr const int* const_first = &array[0];
static_assert((const_first <=> &array[0]) == 0);  // qualification

int count = 0;
int next() { return ++count; }

std::strong_ordering compare(int a, int b) { return a <=> b; }
std::partial_ordering compare(double a, double b) { return a <=> b; }
std::strong_ordering compare(E a, E b) { return a <=> b; }
std::strong_ordering compare(const int* a, const int* b) { return a <=> b; }

int main() {
    if (compare(1, 2) != std::strong_ordering::less) return 1;
    if (compare(2, 2) != std::strong_ordering::equal) return 2;
    if (compare(3, 2) != std::strong_ordering::greater) return 3;
    if (compare(-1.5, 2.0) != std::partial_ordering::less) return 4;
    if (compare(2.0, 2.0) != std::partial_ordering::equivalent) return 5;
    if (compare(4.0, 2.0) != std::partial_ordering::greater) return 6;
    double zero = 0.0;
    if (compare(zero / zero, 2.0) != std::partial_ordering::unordered) return 7;
    if (compare(e2, e1) <= 0) return 8;
    if (compare(&array[1], &array[0]) <= 0) return 9;
    // Each operand is evaluated once.
    if ((next() <=> 100) >= 0 || (100 <=> next()) <= 0 || count != 2) return 10;
    unsigned u = 7;
    long l = -1;
    if ((u <=> 7u) != 0 || (l <=> 0L) >= 0) return 11;
    return 0;
}
