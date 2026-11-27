// A lambda in `decltype` in an alias-declaration and an alias template at namespace scope
// (#1549, [expr.prim.lambda.general]/3). [temp.alias]/5: the closure type differs between the
// template's specializations even where the lambda is not dependent.
template <class A, class B>
struct same {
  static constexpr bool value = false;
};
template <class A>
struct same<A, A> {
  static constexpr bool value = true;
};
template <class T>
using ptr = decltype([] { return static_cast<T*>(nullptr); }());
template <class T>
using pointer_of = decltype([] {
  if constexpr (requires { typename T::pointer; }) return static_cast<typename T::pointer>(nullptr);
  else return static_cast<T*>(nullptr);
}());
template <class T>
using closure = decltype([] {});
using three = decltype([] { return 3; }());
using F = decltype([] { return 7; });
using G = decltype([] { return 7; });
namespace n {
using L = decltype([](long x) { return x; }(1));
}
struct HasPointer {
  using pointer = short*;
};
// Named with dependent arguments, in a class template and a function template.
template <class U>
struct X {
  using c = closure<U>;
  using p = ptr<U>;
};
template <class U>
constexpr bool pointer_to() {
  return same<ptr<U>, U*>::value;
}
static_assert(same<ptr<int>, int*>::value && same<ptr<char>, char*>::value);
static_assert(!same<X<int>::c, X<long>::c>::value && same<X<int>::p, int*>::value && pointer_to<char>());
static_assert(same<pointer_of<HasPointer>, short*>::value && same<pointer_of<int>, int*>::value);
static_assert(!same<closure<int>, closure<char>>::value);
static_assert(same<three, int>::value && same<n::L, long>::value);
static_assert(!same<F, G>::value);
int main() {
  ptr<int> p = nullptr;
  three x = 0;
  // A closure type named outside its lambda-expression: its call operator has code.
  return p ? 1 : F{}() + G{}() - 14 + x;
}
