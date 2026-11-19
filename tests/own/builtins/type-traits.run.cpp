// #211: the type-trait built-ins GCC and Clang both have, as libycxx uses them: predicates
// ([meta.unary.cat], [meta.unary.comp], [meta.unary.prop], [meta.rel]) and transformations
// ([meta.trans]), on non-dependent types and through templates (docs/EXTENSIONS.md).
// EXIT: 0
#if !__has_builtin(__is_same) || !__has_builtin(__remove_cv) || __has_builtin(__no_such_builtin)
#error __has_builtin
#endif
enum E { e0 };
enum class SE : long { s0 };
struct S { int x; };
struct Empty {};
union U { int a; long b; };
template<class T> using RemoveCv = __remove_cv(T);
template<class T> constexpr bool is_ptr = __is_pointer(T);
template<class T, class... Us> constexpr bool any_of = (__is_same(T, Us) || ...);
static_assert(__is_same(int, int) && !__is_same(int, const int) && !__is_same(int, long));
static_assert(__is_same(__remove_cv(const volatile int), int) && __is_same(RemoveCv<const char>, char));
static_assert(__is_same(__remove_cvref(const int&), int));
static_assert(__is_same(__decay(int[3]), int*) && __is_same(__decay(const int&), int) && __is_same(__decay(int(int)), int(*)(int)));
static_assert(__is_same(__add_pointer(int&), int*) && __is_same(__add_lvalue_reference(int&&), int&) &&
              __is_same(__add_lvalue_reference(void), void) && __is_same(__add_rvalue_reference(int&), int&));
static_assert(__is_same(__remove_extent(int[2][3]), int[3]) && __is_same(__remove_all_extents(int[2][3]), int));
static_assert(__is_same(__underlying_type(SE), long) && __is_same(__remove_pointer(int*), int));
static_assert(__is_enum(E) && __is_enum(SE) && !__is_enum(int) && __is_scoped_enum(SE) && !__is_scoped_enum(E));
static_assert(__is_class(S) && !__is_class(U) && __is_union(U) && !__is_union(S));
static_assert(__is_pointer(int*) && !__is_pointer(int) && is_ptr<char*> && !is_ptr<char>);
static_assert(!__is_member_pointer(int*) && !__is_polymorphic(S) && !__is_abstract(S) && !__is_final(S));
static_assert(__is_empty(Empty) && !__is_empty(S) && __is_aggregate(S) && __is_aggregate(int[2]) && !__is_aggregate(int));
static_assert(__is_const(const int) && !__is_const(int) && __is_volatile(volatile int));
static_assert(__is_function(int(int)) && __is_object(int) && !__is_object(int&) && !__is_object(void));
static_assert(__is_reference(int&) && __is_array(int[2]) && __is_bounded_array(int[2]) && !__is_unbounded_array(int[2]));
static_assert(any_of<int, char, int> && !any_of<int, long> && !any_of<int>);
template<class T> int count_pointers(T) { return __is_pointer(T) ? 1 : 0; }
int main() {
  RemoveCv<const int> x = 3;
  int* p = &x;
  return x - 3 + count_pointers(p) - 1 + count_pointers(x);
}
