// θ step 1 (#279), from the review: a variable template whose initializer's type depends on its
// parameter: each specialization has the type its own initializer deduces ([dcl.spec.auto],
// [temp.inst]/7).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };
template<class T> constexpr auto one = T(1);
static_assert(same<decltype(one<char>), const char>::value);
static_assert(one<char> == 1 && sizeof(one<long>) == sizeof(long));
int main() {
  auto x = one<long>;
  int a[one<int> + 1];
  return one<int> + int(x) + int(sizeof a / sizeof a[0]) == 4 ? 0 : 1;
}
