// [dcl.ambig.res]: literal-led and braced conversions initialize objects, while declaration forms remain functions.
struct S {
  int value;
  S(int n) : value(n) {}
};

template<class A, class B> constexpr bool same_type = false;
template<class T> constexpr bool same_type<T, T> = true;

S namespace_parens(S(1));
S namespace_braces(S{1});
static_assert(same_type<decltype(namespace_parens), S>);
static_assert(same_type<decltype(namespace_braces), S>);

S takes_factory(S());
S takes_object(S(argument));
using Factory = S (*)();
using TakesFactory = S(Factory);
using TakesObject = S(S);
static_assert(same_type<decltype(takes_factory), TakesFactory>);
static_assert(same_type<decltype(takes_object), TakesObject>);

int block_case() {
  S b(S(1));
  static_assert(same_type<decltype(b), S>);
  return b.value;
}

int main() {
  return namespace_parens.value == 1 && namespace_braces.value == 1 &&
         block_case() == 1 ? 0 : 1;
}
