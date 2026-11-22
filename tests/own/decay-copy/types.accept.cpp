// [expr.type.conv], [dcl.type.auto.deduct], [temp.deduct.call] (#286).
// Keep the expression in decltype so that a declaration's own deduction cannot hide its type.
const volatile int scalar = 3;
const int* const volatile pointer = nullptr;
const int array[2][3] = {};
int function(int);

static_assert(__is_same(decltype((auto(scalar))), int));
static_assert(__is_same(decltype((auto{scalar})), int));
static_assert(__is_same(decltype((auto(static_cast<const volatile int&&>(scalar)))), int));
static_assert(__is_same(decltype((auto{static_cast<const volatile int&&>(scalar)})), int));
static_assert(__is_same(decltype((auto(pointer))), const int*));
static_assert(__is_same(decltype((auto{pointer})), const int*));
static_assert(__is_same(decltype((auto(array))), const int (*)[3]));
static_assert(__is_same(decltype((auto{array})), const int (*)[3]));
static_assert(__is_same(decltype((auto(function))), int (*)(int)));
static_assert(__is_same(decltype((auto{function})), int (*)(int)));

constexpr int value = 6;
static_assert(auto(value) == 6);
static_assert(auto{value} == 6);
static_assert(auto{value,} == 6);
static_assert(auto((static_cast<void>(0), value)) == 6);
static_assert(auto{(static_cast<void>(0), value)} == 6);
