// #1396: pointers to pointers convert by qualification conversions only ([conv.qual]).
struct C {};
struct B {};
struct D : B {};

static_assert(__is_convertible(int* const*, int* const*));
static_assert(__is_convertible(const C* const*, const C* const*));
static_assert(__is_convertible(int**, int* const*) && __is_convertible(int**, const int* const*));
static_assert(__is_convertible(int**, const volatile int* const volatile*));
static_assert(__is_convertible(int***, const int* const* const*) && __is_convertible(int***, int** const*));
// A level that adds qualifiers needs const on every level above it.
static_assert(!__is_convertible(int**, const int**) && !__is_convertible(int***, const int* const**));
static_assert(__is_convertible(int**, volatile int* const*) && __is_convertible(int**, int* volatile*));
static_assert(!__is_convertible(int* const*, int**) && !__is_convertible(const int**, int**));
// Pointers that differ below their pointer levels are unrelated.
static_assert(!__is_convertible(int**, long**) && !__is_convertible(int**, int*) && !__is_convertible(int*, int**));
static_assert(!__is_convertible(D**, B**) && !__is_convertible(D**, const B* const*));
// Any object pointer converts to `cv void*`, the pointee's qualifiers kept ([conv.ptr]/2).
static_assert(__is_convertible(int**, void*) && __is_convertible(int* const*, const void*));
static_assert(!__is_convertible(int* const*, void*) && !__is_convertible(int***, void**));
static_assert(__is_convertible(int**, bool) && !__is_convertible(int**, int));
static_assert(__is_convertible(decltype(nullptr), int* const*));
static_assert(!__is_convertible(int, int*) && !__is_convertible(int, decltype(nullptr)));

// Function pointers: a noexcept one converts to the plain one, never to `void*` ([conv.fctptr]).
using F = void();
using G = void() noexcept;
static_assert(__is_convertible(G*, F*) && !__is_convertible(F*, G*) && !__is_convertible(F*, void*));
static_assert(__is_convertible(F**, F* const*) && !__is_convertible(G**, F**) && __is_convertible(F*, bool));

static_assert(__is_nothrow_convertible(int**, const int* const*) && !__is_nothrow_convertible(int**, const int**));

// Dependent operands are decided at instantiation ([temp.inst]).
template <class T> constexpr bool to_const = __is_convertible(T**, const T* const*);
template <class T> constexpr bool to_ref = __is_convertible(T*, T* const&) && !__is_convertible(T*, T*&);
static_assert(to_const<int> && to_const<C> && to_ref<void> && to_ref<const C>);

int main() {}
