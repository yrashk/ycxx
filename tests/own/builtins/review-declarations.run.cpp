// #211, from the review: a namespace-scope declaration whose type is a transformation built-in
// (variables, typedefs, functions, variable templates); the cv-qualifiers of arrays, of unknown
// bound too; an incomplete union; and `__decay` of a cv-qualified function type ([meta.trans],
// [meta.unary.prop]).
// EXIT: 0
typedef __remove_cv(const int) TT;
__remove_cv(const int) x = 3;
const __remove_cv(int) y = 4;
namespace N { __remove_cv(const int) v = 5; }
__remove_cv(const int) helper() { return 6; }
template<class E> __remove_cv(E) vt = 7;
extern int arr[];
static_assert(__is_same(__remove_cv(decltype(arr)), decltype(arr)));
static_assert(__is_const(const int[3]) && __is_volatile(volatile int[3]));
union UI;
static_assert(__is_union(UI) && !__is_class(UI) && !__is_empty(UI));
template<class T> struct W { using t = __decay(T); };
static_assert(__is_same(W<int() const>::t, int() const));
int main() {
  TT a = 1;
  return a + x + y + N::v + helper() + vt<int> == 26 ? 0 : 1;
}
