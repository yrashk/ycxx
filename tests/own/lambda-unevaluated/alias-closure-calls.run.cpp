// The closure type of a lambda in an alias template's specialization (#1549, [temp.alias]/5): its
// members are called at run time and in constant expressions, a generic lambda's operator is
// instantiated, a captureless closure converts to a pointer to function, and an alias template
// naming such a specialization has each of its own.
template <class A, class B>
struct same {
  static constexpr bool value = false;
};
template <class A>
struct same<A, A> {
  static constexpr bool value = true;
};
template <class T>
using L = decltype([] { return int(sizeof(T)); });
template <class T>
using G = decltype([](auto x) { return x + T(1); });
template <class T>
using R = decltype([](auto x) { return x; }(T{}));
template <class T>
using B = L<T>;
template <class T>
using P = L<T*>;
namespace n {
template <class T>
using M = decltype([](T x) { return x * 2; });
}
static_assert(L<char>{}() == 1 && L<int>{}() == 4);
static_assert(same<R<long>, long>::value && same<R<char>, char>::value);
// Clang gives each use of a specialization its own closure type, so `same<B<int>, L<int>>` is
// false there (docs/compatibility.toml, alias-template-closure-identity).
static_assert(!same<B<int>, B<char>>::value && !same<P<int>, L<int>>::value);
int main() {
  int (*f)() = L<short>{};
  if (L<char>{}() != 1 || f() != 2 || B<int>{}() != 4 || P<char>{}() != int(sizeof(char*))) return 1;
  if (G<int>{}(2) != 3 || G<long>{}(2.5) != 3.5) return 2;
  return n::M<int>{}(21) == 42 ? 0 : 3;
}
