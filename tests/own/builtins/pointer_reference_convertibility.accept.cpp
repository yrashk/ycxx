// #1396: a reference to a pointer bound to declval<From>() ([meta.rel], [dcl.init.ref]/5).
struct C {};
struct B {};
struct D : B {};

static_assert(__is_convertible(const C*, const C* const&) && __is_convertible(int*, int* const&));
// declval<int*>() is an xvalue: a const lvalue reference or an rvalue reference binds it.
static_assert(__is_convertible(int*, int*&&) && !__is_convertible(int*, int*&));
static_assert(__is_convertible(int*&, int*&) && !__is_convertible(int*&, int*&&));
static_assert(__is_convertible(int*&&, int*&&) && __is_convertible(int*&&, int* const&));
// A reference-compatible lvalue binds directly ([dcl.init.ref]/4: int** to const int* const*).
static_assert(__is_convertible(int*&, const int* const&) && !__is_convertible(int*&, const int*&));
// A related lvalue converts to a temporary only for a const lvalue reference.
static_assert(!__is_convertible(int*&, const int*&&) && __is_convertible(int*, const int*&&));
// cv1 must be at least cv2 for related types ([dcl.init.ref]/5.4.3).
static_assert(!__is_convertible(int* volatile&, int* const&) && !__is_convertible(int* const&, int*&&));
static_assert(!__is_convertible(int* const&, int*&) && !__is_convertible(int*, int* volatile const&));
// Converting to a temporary needs a valid conversion.
static_assert(!__is_convertible(int**&, const int** const&) && __is_convertible(int**&, const int* const* const&));
// Unrelated pointers convert to a temporary of the target type, through a valid conversion.
struct Private : private B {};
struct Left : B {};
struct Right : B {};
struct Both : Left, Right {};
static_assert(!__is_convertible(Private*&, B* const&) && !__is_convertible(Both*, B*&&));
static_assert(__is_convertible(int**, const int* const*&&) && !__is_convertible(int**&, const int* const*&&));
static_assert(__is_convertible(D*&, B* const&) && __is_convertible(D*, B*&&) && !__is_convertible(D*&, B*&));
static_assert(__is_convertible(int*, void* const&) && __is_convertible(int*&, const void* const&));
static_assert(__is_convertible(decltype(nullptr), int* const&) && !__is_convertible(decltype(nullptr), int*&));
static_assert(__is_convertible(int*, const bool&) && !__is_convertible(int*, bool&) && !__is_convertible(int*, const int&));
static_assert(!__is_convertible(int, int* const&) && !__is_convertible(long, const decltype(nullptr)&));
static_assert(__is_convertible(void (*)() noexcept, void (* const&)()));
// Arithmetic references keep their answers.
static_assert(__is_convertible(int, const long&) && !__is_convertible(int&, long&) && __is_convertible(int&, int&));
static_assert(!__is_convertible(int&, int&&) && !__is_convertible(const int&, int&) && __is_convertible(int, int&&));

// Dependent operands in a constraint are decided when it is checked.
template <class T, class U> concept convertible = __is_convertible(T, U);
static_assert(convertible<int*&, const int* const&> && !convertible<int*&, const int*&>);
template <class T> requires(__is_convertible(T*, const T* const&)) constexpr bool binds = true;
static_assert(binds<int> && binds<C>);

static_assert(__is_nothrow_convertible(int*, int* const&) && !__is_nothrow_convertible(int*&, const int*&));

int main() {}
