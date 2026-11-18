// [expr.const], [expr.add]: forming a pointer more than one past the end of an array, or before
// its start, is undefined even without a read, so the expression is not constant. A non-array
// object counts as an array of one element.
// XFAIL: gcc GCC 16 accepts out-of-range pointer arithmetic that is not dereferenced; [expr.add]p4 makes it undefined
constexpr const char* text() { return "ab"; }
constexpr bool offset(int k) { int a[4]; int* p = a + k; return p != nullptr; }
static_assert(text() + 3 != nullptr);
static_assert(offset(0) && offset(4));
static_assert(text() + 4 != nullptr); //! error
static_assert(text() - 1 != nullptr); //! error
constexpr bool too_far() { int a[4]; int* p = a + 5; return p != nullptr; } static_assert(too_far()); //! error
constexpr bool too_soon() { int a[4]; int* p = a - 1; return p != nullptr; } static_assert(too_soon()); //! error
constexpr bool two_past() { int x = 0; int* p = &x + 2; return p != nullptr; } static_assert(two_past()); //! error
