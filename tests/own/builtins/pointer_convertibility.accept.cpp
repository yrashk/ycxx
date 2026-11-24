// #218: object pointer and std::nullptr_t sources ([conv.ptr], [conv.qual], [conv.bool]).
struct B {};
struct D : B {};
struct Private : private B {};
struct Left : B {};
struct Right : B {};
struct Both : Left, Right {};
struct Incomplete;
union U {};

static_assert(__is_convertible(D*, B*) && __is_convertible(D*, const B*));
static_assert(!__is_convertible(const D*, B*) && !__is_convertible(B*, D*));
static_assert(!__is_convertible(Private*, B*) && !__is_convertible(Both*, B*));
static_assert(__is_convertible(Both*, Left*));
static_assert(!__is_convertible(Incomplete*, B*) && __is_convertible(Incomplete*, const Incomplete*));
static_assert(__is_convertible(D*, void*) && __is_convertible(D* const&, const void*));
static_assert(!__is_convertible(const D*, void*) && !__is_convertible(U*, B*));
static_assert(__is_convertible(int*, const volatile int*) && !__is_convertible(int*, long*));
static_assert(__is_convertible(int*, bool) && !__is_convertible(int*, int));
static_assert(__is_convertible(decltype(nullptr), int*) && !__is_convertible(decltype(nullptr), bool));
static_assert(__is_nothrow_convertible(D*, B*) && !__is_nothrow_convertible(Private*, B*));

// Virtual functions do not change the bases or the access to them.
struct Abstract { virtual void f() = 0; };
struct Concrete : Abstract { void f() override {} };
static_assert(__is_convertible(Concrete*, Abstract*) && !__is_convertible(Abstract*, Concrete*));

// An array's qualifiers are its elements' ([basic.type.qualifier]/3).
static_assert(__is_convertible(int(*)[3], const int(*)[3]));
static_assert(!__is_convertible(const int(*)[3], void*) && !__is_convertible(const int(*)[2][3], void*));
static_assert(__is_convertible(const int(*)[2][3], const void*));

int main() {}
